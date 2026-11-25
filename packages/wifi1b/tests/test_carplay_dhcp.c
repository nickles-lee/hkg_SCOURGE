/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * DHCP client release-gate tests: lease timers, client identity, AF_PACKET
 * parsing and owned default-route policy. No host network or firewall is
 * touched; namespace modes are run by test_carplay_dhcp.py.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }

static char g_route_defaults[2048];
static char g_wlan_defaults[2048];
static char g_route_after_add[2048];
static char g_route_after_del[2048];
static bool g_fail_route_add;
static bool g_fail_route_del;
static bool g_fail_addr_query;
static bool g_fail_addr_del;
static bool g_fail_route_query;
static int g_route_query_fail_after = -1;
static int g_route_query_count;
static int64_t g_fake_now_ms;
static int64_t g_advance_on_addr_apply;
static char g_addr_state[512];
static char g_connected_state[512];
static bool g_modem_ipv4;
static bool g_modem_unknown;
static char g_cmds[64][512];
static int g_cmd_count;

static int fixture_command(char *out, size_t n, const char *cmd);
static void wait_for_path(const char *path);
static void reap_until_idle(void);

static void fixture_reset(void)
{
    g_route_defaults[0] = 0;
    g_wlan_defaults[0] = 0;
    g_route_after_add[0] = 0;
    g_route_after_del[0] = 0;
    g_fail_route_add = false;
    g_fail_route_del = false;
    g_fail_addr_query = false;
    g_fail_addr_del = false;
    g_fail_route_query = false;
    g_route_query_fail_after = -1;
    g_route_query_count = 0;
    g_fake_now_ms = 0;
    g_advance_on_addr_apply = 0;
    g_addr_state[0] = 0;
    g_connected_state[0] = 0;
    g_modem_ipv4 = false;
    g_modem_unknown = false;
    g_client_modem_ipv4 = false;
    g_client_modem_unknown = false;
    g_cmd_count = 0;
    test_command = fixture_command;
    test_owner_write_fail = 0;
    test_owner_dir_sync_fail = 0;
    test_txn_budget_ms = 0;
    g_dhcp_owner_fs_unconfirmed = false;
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
}

static void write_test_route_ledger(const char *gw, const char *iface,
                                    unsigned int metric, unsigned int proto)
{
    assert(w2_route_ledger_publish(gw, iface, metric, proto));
}

static int fixture_command(char *out, size_t n, const char *cmd)
{
    if (g_cmd_count < 64)
        snprintf(g_cmds[g_cmd_count++], sizeof(g_cmds[0]), "%s", cmd);
    if (out && n) out[0] = 0;
    if (strstr(cmd, "ip -o -4 addr show dev wlan0")) {
        if (g_fail_addr_query) return 1;
        snprintf(out, n, "%s", g_addr_state);
        return 0;
    }
    if (strstr(cmd, "ip -4 route show dev wlan0")) {
        snprintf(out, n, "%s", g_connected_state);
        return 0;
    }
    if (strstr(cmd, "ip -4 route show default")) {
        if (g_fail_route_query) return 1;
        if (g_route_query_fail_after >= 0 &&
            g_route_query_count++ >= g_route_query_fail_after)
            return 1;
        /* The target iproute2 omits `dev wlan0` when the query is filtered;
         * the daemon deliberately queries the unfiltered table. */
        snprintf(out, n, "%s%s", g_route_defaults, g_wlan_defaults);
        return 0;
    }
    if (strstr(cmd, "ip -o -4 addr show dev rmnet_usb0") ||
        strstr(cmd, "ip -o -4 addr show dev rmnet_usb1")) {
        if (g_modem_unknown) return 1;
        if (g_modem_ipv4)
            snprintf(out, n, "5: rmnet_usb0    inet 198.51.100.135/28 brd 198.51.100.143 scope global rmnet_usb0\n");
        return 0;
    }
    if (strstr(cmd, "ifconfig rmnet_usb0") ||
        strstr(cmd, "ifconfig rmnet_usb1")) {
        if (g_modem_unknown) return 1;
        if (g_modem_ipv4 && out && n)
            snprintf(out, n, "rmnet_usb0: flags=4163<UP>  inet 198.51.100.135  netmask 255.255.255.240\n");
        return 0;
    }
    if (strstr(cmd, "ip link show dev rmnet_usb0") ||
        strstr(cmd, "ip link show dev rmnet_usb1")) {
        return g_modem_unknown ? 1 : 0;
    }
    if (strstr(cmd, "ip link show") != NULL) {
        if ((g_modem_ipv4 || g_modem_unknown) && out && n)
            snprintf(out, n, "5: rmnet_usb0: <UP> mtu 1500\n");
        return 0;
    }
    if (strstr(cmd, "route del default") && strstr(cmd, "dev wlan0")) {
        if (g_route_after_del[0])
            snprintf(g_route_defaults, sizeof(g_route_defaults), "%s", g_route_after_del);
        else
            g_wlan_defaults[0] = 0;
        return 0;
    }
    if (strstr(cmd, "route add default")) {
        if (g_fail_route_add) return 1;
        if (g_route_after_add[0]) snprintf(g_route_defaults, sizeof(g_route_defaults), "%s", g_route_after_add);
        return 0;
    }
    if (strstr(cmd, "route del default")) {
        if (g_fail_route_del) return 1;
        if (g_route_after_del[0]) snprintf(g_route_defaults, sizeof(g_route_defaults), "%s", g_route_after_del);
        return 0;
    }
    if (strstr(cmd, "addr replace 10.42.0.20/24")) {
        snprintf(g_addr_state, sizeof(g_addr_state),
                 "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
        snprintf(g_connected_state, sizeof(g_connected_state),
                 "10.42.0.0/24 dev wlan0 proto kernel scope link src 10.42.0.20\n");
        if (g_advance_on_addr_apply) {
            g_fake_now_ms += g_advance_on_addr_apply;
            g_advance_on_addr_apply = 0;
        }
        return 0;
    }
    if (strstr(cmd, "addr replace 10.42.0.20/31") ||
        strstr(cmd, "addr add 10.42.0.20/31")) {
        snprintf(g_addr_state, sizeof(g_addr_state),
                 "3: wlan0    inet 10.42.0.20/31 brd 10.42.0.21 scope global wlan0\n");
        snprintf(g_connected_state, sizeof(g_connected_state),
                 "10.42.0.20/31 dev wlan0 proto kernel scope link src 10.42.0.20\n");
        return 0;
    }
    if (strstr(cmd, "addr del")) {
        if (!g_fail_addr_del) g_addr_state[0] = 0;
        return 0;
    }
    return 0;
}

static bool command_seen(const char *needle)
{
    for (int i = 0; i < g_cmd_count; i++)
        if (strstr(g_cmds[i], needle)) return true;
    return false;
}

static int64_t fake_now_ms(void)
{
    return g_fake_now_ms;
}

static bool cleanup_test_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind == WORK_DHCP || kind == WORK_MODE) {
        return w2_dhcp_cleanup_owned("wlan0", true);
    }
    return true;
}

static void write_text(const char *path, const char *text);
static void write_private(const char *path, const char *text)
{
    write_text(path, text);
    assert(chmod(path, 0600) == 0);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void test_masks_and_options(void)
{
    uint8_t opts[312], mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    size_t discover_len, request_len;
    uint32_t value = 0;
    struct w2_dhcp_packet packet;

    assert(w2_mask_to_prefix(0xffffff00U) == 24);
    assert(w2_mask_to_prefix(0xffffffffU) == 32);
    assert(w2_mask_to_prefix(0x80000000U) == 1);
    assert(w2_mask_to_prefix(0xfffffffeU) == 31);
    assert(w2_mask_to_prefix(0xffff00ffU) == -1);

    memset(opts, 0, sizeof(opts));
    discover_len = w2_dhcp_client_opts(opts, sizeof(opts), 1, mac, 0, 0, false);
    uint8_t *cid1 = w2_dhcp_opt(opts, discover_len, 61);
    assert(cid1 != NULL && cid1[0] == 1 && !memcmp(cid1 + 1, mac, 6));
    uint8_t cid1_copy[7];
    memcpy(cid1_copy, cid1, 7);
    memset(opts, 0, sizeof(opts));
    request_len = w2_dhcp_client_opts(opts, sizeof(opts), 3, mac,
                                      w2_ip4("10.42.0.10"),
                                      w2_ip4(WIFI1B_AP_IP), true);
    uint8_t *cid2 = w2_dhcp_opt(opts, request_len, 61);
    assert(cid2 != NULL && cid2[0] == 1 && !memcmp(cid2 + 1, mac, 6));
    assert(!memcmp(cid1_copy, cid2, 7));

    memset(&packet, 0, sizeof(packet));
    packet.options[0] = 99; packet.options[1] = 130;
    packet.options[2] = 83; packet.options[3] = 99;
    uint8_t type = 5;
    size_t off = 4;
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 53, 1, &type);
    uint32_t lease = htonl(7200);
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 51, 4, &lease);
    uint32_t t1 = htonl(3600);
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 58, 4, &t1);
    uint32_t t2 = htonl(6300);
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 59, 4, &t2);
    uint32_t router = htonl(w2_ip4(WIFI1B_AP_IP));
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 3, 4, &router);
    packet.options[off++] = 255;
    size_t n = offsetof(struct w2_dhcp_packet, options) + off;
    assert(w2_dhcp_opt_u32(&packet, n, 51, &value) && value == 7200);
    assert(w2_dhcp_opt_u32(&packet, n, 58, &value) && value == 3600);
    assert(w2_dhcp_opt_u32(&packet, n, 59, &value) && value == 6300);
    assert(w2_dhcp_opt_u32(&packet, n, 3, &value) && value == w2_ip4(WIFI1B_AP_IP));
    assert(!w2_dhcp_opt_u32(&packet, n, 54, &value));
}

static void test_lease_action(void)
{
    int64_t now = 100000;
    assert(dhcp_lease_action(false, now, 0, 0, 0) == DHCP_TICK_ACQUIRE);
    assert(dhcp_lease_action(true, now, 200000, 300000, 400000) == DHCP_TICK_NONE);
    assert(dhcp_lease_action(true, 200000, 200000, 300000, 400000) == DHCP_TICK_RENEW);
    assert(dhcp_lease_action(true, 300000, 200000, 300000, 400000) == DHCP_TICK_REBIND);
    assert(dhcp_lease_action(true, 400000, 200000, 300000, 400000) == DHCP_TICK_ACQUIRE);
    assert(dhcp_lease_action(true, now, 0, 0, 400000) == DHCP_TICK_NONE);
}

static void test_route_policy_units(void)
{
    unsigned int metric = 0;
    w2_owned_route_t route;
    char ledger[512];

    fixture_reset();
    g_route_defaults[0] = 0;
    assert(w2_route_select_metric(&metric) && metric == WIFI1B_CLIENT_DEFAULT_METRIC);

    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(w2_route_select_metric(&metric) && metric == 101);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 500\n");
    assert(w2_route_select_metric(&metric) && metric == 501);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static\n");
    assert(w2_route_select_metric(&metric) && metric == 1);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 metric 4294967295\n");
    assert(!w2_route_select_metric(&metric));
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 metric 12x\n");
    assert(!w2_route_select_metric(&metric));

    assert(w2_route_line_matches(
        "default via 192.0.2.1 dev modem0 proto 4 metric 100",
        "192.0.2.1", "modem0", 100, 4));
    assert(!w2_route_line_matches(
        "default via 192.0.2.1 dev modem0 proto 4 metric 100",
        "192.0.2.1", "modem0", 101, 4));

    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    assert(w2_route_ledger_publish("10.42.0.1", "wlan0", 101, 66));
    assert(read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0);
    assert(w2_owned_route_parse(ledger, &route));
    assert(!strcmp(route.gateway, "10.42.0.1"));
    assert(!strcmp(route.iface, "wlan0"));
    assert(route.metric == 101 && route.proto == 66);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    write_text(WIFI1B_CLIENT_ROUTE_LEDGER, "v1 10.42.0.1 wlan0 101 66\n");
    assert(!w2_owned_route_parse("v1 10.42.0.1 wlan0 101 66\n", &route));
    assert(!w2_owned_route_parse("v2 scope=wifi1b-dhcp-v1 lineage=bad gen=1 pid=1 gw=10.42.0.1 iface=wlan0 metric=101 proto=66\n", &route));
    assert(!w2_owned_route_parse("v2 scope=wifi1b-dhcp-v1 lineage=00000000000000000000000000000000 gen=1 pid=1 gw=192.0.2.2 iface=modem0 metric=100 proto=4\n", &route));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
}

static void test_route_install_cleanup(void)
{
    fixture_reset();
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 10.42.0.1 dev lo proto 66 metric 101\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(g_dhcp_lease.route_ok);
    assert(g_dhcp_lease.route_metric == 101);
    assert(g_dhcp_lease.route_proto == WIFI1B_CLIENT_ROUTE_PROTO);
    assert(command_seen("route add default via 10.42.0.1 dev lo metric 101 proto 66"));
    char ledger[1024];
    assert(read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0);
    assert(strstr(ledger, "scope=wifi1b-dhcp-v1"));
    assert(strstr(ledger, "gw=10.42.0.1") && strstr(ledger, "iface=lo"));
    assert(strstr(ledger, "metric=101") && strstr(ledger, "proto=66"));
    g_cmd_count = 0;
    assert(w2_owned_route_cleanup());
    assert(command_seen("route del default via 10.42.0.1 dev lo metric 101 proto 66"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
    assert(!command_seen("route flush"));
    assert(!command_seen("modem0"));

    /* Installation/readback failure must be false and leave no ledger. */
    fixture_reset();
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    g_fail_route_add = true;
    assert(!w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);

    fixture_reset();
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(!w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);

    /* A corrupt ledger is never acted on. */
    fixture_reset();
    write_text(WIFI1B_CLIENT_ROUTE_LEDGER, "v1 bad ledger\n");
    g_cmd_count = 0;
    assert(!w2_owned_route_cleanup());
    assert(g_cmd_count == 0);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
}

static void test_legacy_cleanup(void)
{
    fixture_reset();
    snprintf(g_wlan_defaults, sizeof(g_wlan_defaults),
             "default via 198.51.100.1 dev wlan0\n");
    g_cmd_count = 0;
    assert(!w2_legacy_wlan_default_cleanup());
    assert(strstr(g_wlan_defaults, "198.51.100.1") != NULL);
    assert(!command_seen("route del default"));
    assert(!command_seen("route add default"));
}

static void test_modem_safe_route_policy(void)
{
    fixture_reset();
    config_defaults();
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
    g_modem_ipv4 = true;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static\n");
    g_cmd_count = 0;
    assert(w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(g_dhcp_lease.route_ok);
    assert(!g_dhcp_lease.wifi_default);
    assert(g_dhcp_lease.route_metric == 0 && g_dhcp_lease.route_proto == 0);
    assert(g_client_modem_ipv4);
    assert(!command_seen("route add default"));

    /* An unverified V21/foreign wlan0 default is preserved, not deleted. */
    fixture_reset();
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
    g_modem_ipv4 = true;
    snprintf(g_wlan_defaults, sizeof(g_wlan_defaults),
             "default via 198.51.100.1 dev wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static\n");
    g_cmd_count = 0;
    assert(!w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(strstr(g_wlan_defaults, "198.51.100.1") != NULL);
    assert(!command_seen("route del default"));
    assert(!g_dhcp_lease.wifi_default);

    /* Explicit fallback policy still installs below the modem default. */
    fixture_reset();
    g_cfg.client_route_policy = CLIENT_ROUTE_FALLBACK;
    g_modem_ipv4 = true;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 10.42.0.1 dev lo proto 66 metric 101\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(g_dhcp_lease.route_ok && g_dhcp_lease.wifi_default);
    assert(g_dhcp_lease.route_metric == 101);
    assert(w2_owned_route_cleanup());
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);

    /* Unknown modem readback is not "modem down": no fallback is added and
     * no owned/foreign route is deleted. */
    fixture_reset();
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
    g_modem_unknown = true;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    g_cmd_count = 0;
    assert(!w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(!command_seen("route add default"));
    assert(!command_seen("route del default"));
    assert(g_client_modem_unknown);
    g_modem_unknown = false;
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
}

static void test_route_audit(void)
{
    fixture_reset();
    config_defaults();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = 66;
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "lo");
    write_test_route_ledger("10.42.0.1", "lo", 101, 66);

    g_cfg.client_route_policy = CLIENT_ROUTE_FALLBACK;
    g_modem_ipv4 = true;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 10.42.0.1 dev lo proto 66 metric 101\n");
    assert(w2_client_route_audit());
    assert(g_dhcp_lease.wifi_default && g_dhcp_lease.route_metric == 101);

    /* A foreign route that outranks the owned fallback forces a safe bump. */
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 192.0.2.9 dev modem1 proto static metric 500\n"
             "default via 10.42.0.1 dev lo proto 66 metric 101\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 192.0.2.9 dev modem1 proto static metric 500\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 192.0.2.9 dev modem1 proto static metric 500\n"
             "default via 10.42.0.1 dev lo proto 66 metric 501\n");
    assert(w2_client_route_audit());
    assert(g_dhcp_lease.wifi_default && g_dhcp_lease.route_metric == 501);

    /* An unverified foreign wlan0 default is preserved and audit fails
     * closed; only an exact ledged route may ever be deleted. */
    snprintf(g_wlan_defaults, sizeof(g_wlan_defaults),
             "default via 198.51.100.200 dev wlan0 metric 0\n");
    assert(!w2_client_route_audit());
    assert(strstr(g_wlan_defaults, "198.51.100.200") != NULL);
    assert(!command_seen("route del default via 198.51.100.200 dev wlan0 metric 0"));
    g_wlan_defaults[0] = 0;

    /* modem_safe removes the owned route as soon as modem data is present. */
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
    g_modem_ipv4 = true;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 10.42.0.1 dev lo proto 66 metric 501\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(w2_client_route_audit());
    assert(!g_dhcp_lease.wifi_default);
    assert(g_dhcp_lease.route_metric == 0 && g_dhcp_lease.route_proto == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);

    /* No modem data -> a missing fallback is reinstated. */
    g_modem_ipv4 = false;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 10.42.0.1 dev lo proto 66 metric 101\n");
    assert(w2_client_route_audit());
    assert(g_dhcp_lease.wifi_default && g_dhcp_lease.route_metric == 101);
    assert(w2_owned_route_cleanup());
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;

    /* The audit refuses to add/delete anything when modem state is unknown. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = 66;
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "lo");
    write_test_route_ledger("10.42.0.1", "lo", 101, 66);
    g_cfg.client_route_policy = CLIENT_ROUTE_FALLBACK;
    g_modem_unknown = true;
    g_cmd_count = 0;
    assert(!w2_client_route_audit());
    assert(!command_seen("route add default"));
    assert(!command_seen("route del default"));
    assert(g_client_modem_unknown);
    g_modem_unknown = false;
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t build_raw_reply(uint8_t *packet, size_t cap, uint32_t xid,
                              const uint8_t mac[6], bool bad_ip,
                              bool bad_udp, bool fragmented, bool zero_udp)
{
    struct iphdr *ip = (struct iphdr *)packet;
    struct udphdr *udp = (struct udphdr *)(packet + sizeof(*ip));
    struct w2_dhcp_packet *dhcp = (struct w2_dhcp_packet *)(packet + sizeof(*ip) + sizeof(*udp));
    uint8_t opts[128];
    size_t off = 0;
    uint8_t type = 2, cid[7];
    uint32_t sid = htonl(w2_ip4(WIFI1B_AP_IP));
    uint32_t mask = htonl(0xffffff00U);
    uint32_t router = htonl(w2_ip4(WIFI1B_AP_IP));
    uint32_t lease = htonl(3600);

    memset(packet, 0, cap);
    memset(dhcp, 0, sizeof(*dhcp));
    dhcp->op = 2; dhcp->htype = 1; dhcp->hlen = 6; dhcp->xid = xid;
    dhcp->yiaddr = htonl(w2_ip4("10.42.0.10"));
    memcpy(dhcp->chaddr, mac, 6);
    dhcp->options[0] = 99; dhcp->options[1] = 130;
    dhcp->options[2] = 83; dhcp->options[3] = 99;
    cid[0] = 1; memcpy(cid + 1, mac, 6);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 53, 1, &type);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 54, 4, &sid);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 1, 4, &mask);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 3, 4, &router);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 51, 4, &lease);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 61, 7, cid);
    opts[off++] = 255;
    memcpy(dhcp->options + 4, opts, off);
    size_t dhcp_len = offsetof(struct w2_dhcp_packet, options) + 4 + off;
    size_t udp_len = sizeof(*udp) + dhcp_len;
    size_t total = sizeof(*ip) + udp_len;

    ip->version = 4; ip->ihl = 5; ip->tot_len = htons((uint16_t)total);
    ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = htonl(w2_ip4(WIFI1B_AP_IP));
    ip->daddr = htonl(w2_ip4("10.42.0.10"));
    ip->frag_off = htons(fragmented ? 0x2000 : 0);
    ip->check = 0;
    ip->check = htons(checksum_finish(w2_sum_bytes((uint8_t *)ip, sizeof(*ip), 0)));
    if (bad_ip) ip->check ^= 0xffffU;

    udp->source = htons(w2_dhcp_port());
    udp->dest = htons(w2_dhcp_client_port());
    udp->len = htons((uint16_t)udp_len);
    udp->check = 0;
    uint32_t sum = 0;
    sum = w2_sum_bytes((uint8_t *)&ip->saddr, 4, sum);
    sum = w2_sum_bytes((uint8_t *)&ip->daddr, 4, sum);
    sum += ip->protocol;
    sum += (uint32_t)udp_len;
    sum = w2_sum_bytes((uint8_t *)udp, udp_len, sum);
    udp->check = htons(checksum_finish(sum));
    if (zero_udp) udp->check = 0;
    if (bad_udp) udp->check ^= 0xffffU;
    return total;
}

static void set_ip_checksum(uint8_t *packet)
{
    struct iphdr *ip = (struct iphdr *)packet;
    ip->check = 0;
    ip->check = htons(checksum_finish(w2_sum_bytes((uint8_t *)ip, sizeof(*ip), 0)));
}

static void test_raw_parser(void)
{
    uint8_t packet[1500], payload[1500];
    size_t payload_len = 0;
    uint32_t xid = 0x12345678;
    uint8_t mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    size_t n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);

    assert(w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    assert(payload_len >= offsetof(struct w2_dhcp_packet, options) + 4);
    assert(w2_dhcp_msgtype((struct w2_dhcp_packet *)payload, payload_len) == 2);

    n = build_raw_reply(packet, sizeof(packet), xid, mac, true, false, false, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, true, false, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, true);
    assert(w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, true, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    assert(!w2_dhcp_raw_parse(packet, n - 1, xid, mac, payload, sizeof(payload), &payload_len));
    assert(!w2_dhcp_raw_parse(packet, n, xid + 1, mac, payload, sizeof(payload), &payload_len));
    mac[0] ^= 1;
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    mac[0] ^= 1;

    struct w2_dhcp_packet *dhcp = (struct w2_dhcp_packet *)(packet + sizeof(struct iphdr) + sizeof(struct udphdr));
    uint8_t *cid = w2_dhcp_opt(dhcp->options + 4,
                               n - sizeof(struct iphdr) - sizeof(struct udphdr) -
                               offsetof(struct w2_dhcp_packet, options) - 4, 61);
    assert(cid != NULL);
    cid[1] ^= 1;
    struct udphdr *mut_udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    mut_udp->check = 0;
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    /* Wrong UDP destination port. */
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    struct udphdr *udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    udp->dest = htons(9999);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    /* Wrong IP protocol/version, wrong DHCP op/magic, small payload cap. */
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    struct iphdr *ip = (struct iphdr *)packet;
    ip->protocol = IPPROTO_TCP;
    set_ip_checksum(packet);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    ip = (struct iphdr *)packet;
    ip->version = 6;
    set_ip_checksum(packet);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    struct w2_dhcp_packet *bad_dhcp = (struct w2_dhcp_packet *)
        (packet + sizeof(struct iphdr) + sizeof(struct udphdr));
    bad_dhcp->op = 1;
    udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    udp->check = 0;
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    bad_dhcp = (struct w2_dhcp_packet *)
        (packet + sizeof(struct iphdr) + sizeof(struct udphdr));
    bad_dhcp->options[0] = 0;
    udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    udp->check = 0;
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, 100, &payload_len));

    /* A reply without option 61 remains valid; only chaddr/xid are then
     * available for identity, and the checks still reject a foreign client. */
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    bad_dhcp = (struct w2_dhcp_packet *)
        (packet + sizeof(struct iphdr) + sizeof(struct udphdr));
    uint8_t *opt = w2_dhcp_opt(bad_dhcp->options + 4,
                               n - sizeof(struct iphdr) - sizeof(struct udphdr) -
                               offsetof(struct w2_dhcp_packet, options) - 4, 61);
    assert(opt != NULL);
    opt[-2] = 60; /* same length, no client-id option */
    udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    udp->check = 0;
    assert(w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
}

static void test_accept_lease_result(void)
{
    int64_t before = now_ms();
    char buf[2048];
    g_apply_gen = 42;
    g_work_kind = WORK_DHCP;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    snprintf(buf, sizeof(buf),
             "42 1\nRenewed\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_server=10.42.0.1\n"
             "client_lease_gateway=10.42.0.1\nclient_lease_prefix=24\n"
             "client_lease_lease_s=3600\nclient_lease_t1_s=1800\n"
             "client_lease_t2_s=3150\nclient_lease_route_ok=1\n"
             "client_route_metric=101\nclient_route_proto=66\n"
             "client_lease_iface=wlan0\n");
    assert(accept_apply_result(buf));
    assert(g_dhcp_lease.valid && g_dhcp_lease.route_ok);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(g_dhcp_lease.gateway == w2_ip4("10.42.0.1"));
    assert(g_client_t1_at_ms >= before + 1800000);
    assert(g_client_t2_at_ms >= before + 3150000);
    assert(g_client_expiry_at_ms >= before + 3600000);

    char stale[256];
    snprintf(stale, sizeof(stale),
             "41 1\nstale\nclient_lease_updated=1\nclient_lease_valid=1\n");
    assert(!accept_apply_result(stale));

    /* modem-safe suppression: route_ok true, no WiFi default, but the
     * lease router must still be a usable on-subnet gateway. */
    g_apply_gen = 43;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    snprintf(buf, sizeof(buf),
             "43 1\nModem route preserved\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_gateway=10.42.0.1\n"
             "client_lease_prefix=24\nclient_lease_lease_s=3600\n"
             "client_lease_t1_s=1800\nclient_lease_t2_s=3150\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_modem_ipv4=1\nclient_legacy_route_removed=0\n"
             "client_route_metric=0\nclient_route_proto=0\n"
             "client_lease_iface=wlan0\n");
    assert(accept_apply_result(buf));
    assert(g_dhcp_lease.valid && g_dhcp_lease.route_ok);
    assert(!g_dhcp_lease.wifi_default && g_client_modem_ipv4);
    assert(g_client_legacy_routes_removed == 0);
    assert(g_dhcp_lease.gateway == w2_ip4("10.42.0.1"));

    /* A suppressed route is not a missing router: empty, zero, unusable or
     * off-subnet gateways are not ready leases. */
    g_apply_gen = 46;
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = false;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    snprintf(buf, sizeof(buf),
             "46 1\nmissing router\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_gateway=\n"
             "client_lease_prefix=24\nclient_lease_lease_s=3600\n"
             "client_lease_t1_s=1800\nclient_lease_t2_s=3150\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_modem_ipv4=1\nclient_route_metric=0\n"
             "client_route_proto=0\nclient_lease_iface=wlan0\n");
    assert(!accept_apply_result(buf));
    snprintf(buf, sizeof(buf),
             "46 1\noff-subnet router\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_gateway=192.0.2.9\n"
             "client_lease_prefix=24\nclient_lease_lease_s=3600\n"
             "client_lease_t1_s=1800\nclient_lease_t2_s=3150\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_modem_ipv4=1\nclient_route_metric=0\n"
             "client_route_proto=0\nclient_lease_iface=wlan0\n");
    assert(!accept_apply_result(buf));
    snprintf(buf, sizeof(buf),
             "46 1\nzero router\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_gateway=0.0.0.0\n"
             "client_lease_prefix=24\nclient_lease_lease_s=3600\n"
             "client_lease_t1_s=1800\nclient_lease_t2_s=3150\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_modem_ipv4=1\nclient_route_metric=0\n"
             "client_route_proto=0\nclient_lease_iface=wlan0\n");
    assert(!accept_apply_result(buf));
    snprintf(buf, sizeof(buf),
             "46 1\nunknown modem ready\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_gateway=10.42.0.1\n"
             "client_lease_prefix=24\nclient_lease_lease_s=3600\n"
             "client_lease_t1_s=1800\nclient_lease_t2_s=3150\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_modem_ipv4=0\nclient_modem_unknown=1\n"
             "client_route_metric=0\nclient_route_proto=0\n"
             "client_lease_iface=wlan0\n");
    assert(!accept_apply_result(buf));
    /* A nonzero legacy-removal claim is not accepted as proof. */
    snprintf(buf, sizeof(buf),
             "46 1\nfabricated legacy proof\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_gateway=10.42.0.1\n"
             "client_lease_prefix=24\nclient_lease_lease_s=3600\n"
             "client_lease_t1_s=1800\nclient_lease_t2_s=3150\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_modem_ipv4=1\nclient_legacy_route_removed=1\n"
             "client_route_metric=0\nclient_route_proto=0\n"
             "client_lease_iface=wlan0\n");
    assert(accept_apply_result(buf));
    assert(g_client_legacy_routes_removed == 0);

    g_apply_gen = 44;
    snprintf(buf, sizeof(buf),
             "44 1\nbad lease\nclient_lease_updated=1\nclient_lease_valid=1\n");
    assert(!accept_apply_result(buf));
    assert(g_dhcp_lease.valid); /* malformed ready lease preserves the old one */

    snprintf(buf, sizeof(buf),
             "44 1\nclean failure\nclient_lease_updated=1\n"
             "client_lease_valid=0\n");
    assert(accept_apply_result(buf));
    assert(!g_dhcp_lease.valid && !g_dhcp_lease.route_ok);
    assert(g_client_expiry_at_ms == 0);

    /* A timeout result (updated=0) leaves the current clock untouched. */
    g_apply_gen = 45;
    g_dhcp_lease.valid = true;
    g_client_t1_at_ms = 1234;
    g_client_t2_at_ms = 5678;
    g_client_expiry_at_ms = 9999;
    snprintf(buf, sizeof(buf),
             "45 0\ntimeout\nclient_lease_updated=0\n");
    assert(accept_apply_result(buf));
    assert(g_dhcp_lease.valid);
    assert(g_client_t1_at_ms == 1234 && g_client_t2_at_ms == 5678 &&
           g_client_expiry_at_ms == 9999);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
}

static void test_route_audit_result_merge(void)
{
    fixture_reset();
    config_defaults();
    g_fake_now_ms = 5000000;
    test_now_ms = fake_now_ms;
    g_apply_gen = 60;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.route_metric = 100;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_client_t1_at_ms = g_fake_now_ms + 1000;
    g_client_t2_at_ms = g_fake_now_ms + 2000;
    g_client_expiry_at_ms = g_fake_now_ms + 100000;

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "60 1\naudit\nclient_route_audit=1\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_route_metric=0\nclient_route_proto=0\n"
             "client_modem_ipv4=1\nclient_modem_unknown=0\n"
             "client_legacy_route_removed=0\n");
    assert(accept_route_audit_result(buf));
    assert(g_dhcp_lease.valid && g_dhcp_lease.route_ok);
    assert(!g_dhcp_lease.wifi_default);
    assert(g_dhcp_lease.route_metric == 0 && g_dhcp_lease.route_proto == 0);
    assert(g_client_t1_at_ms == g_fake_now_ms + 1000);
    assert(g_client_t2_at_ms == g_fake_now_ms + 2000);
    assert(g_client_expiry_at_ms == g_fake_now_ms + 100000);

    snprintf(buf, sizeof(buf),
             "60 1\naudit fallback\nclient_route_audit=1\n"
             "client_lease_route_ok=1\nclient_wifi_default=1\n"
             "client_route_metric=101\nclient_route_proto=66\n"
             "client_modem_ipv4=0\nclient_modem_unknown=0\n"
             "client_legacy_route_removed=0\n");
    assert(accept_route_audit_result(buf));
    assert(g_dhcp_lease.wifi_default && g_dhcp_lease.route_metric == 101);
    assert(g_client_expiry_at_ms == g_fake_now_ms + 100000);

    snprintf(buf, sizeof(buf),
             "60 1\nnot an audit\nclient_route_audit=0\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_route_metric=0\nclient_route_proto=0\n");
    assert(!accept_route_audit_result(buf));
    snprintf(buf, sizeof(buf),
             "60 1\nbad route readback\nclient_route_audit=1\n"
             "client_lease_route_ok=0\nclient_wifi_default=0\n"
             "client_route_metric=0\nclient_route_proto=0\n");
    assert(!accept_route_audit_result(buf));
    snprintf(buf, sizeof(buf),
             "60 1\nunknown modem\nclient_route_audit=1\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_route_metric=0\nclient_route_proto=0\n"
             "client_modem_unknown=1\n");
    assert(!accept_route_audit_result(buf));

    g_client_expiry_at_ms = g_fake_now_ms - 1;
    snprintf(buf, sizeof(buf),
             "60 1\nexpired\nclient_route_audit=1\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_route_metric=0\nclient_route_proto=0\n");
    assert(!accept_route_audit_result(buf));
    g_client_expiry_at_ms = g_fake_now_ms + 100000;

    g_dhcp_lease.gateway = 0;
    snprintf(buf, sizeof(buf),
             "60 1\nmissing parent router\nclient_route_audit=1\n"
             "client_lease_route_ok=1\nclient_wifi_default=0\n"
             "client_route_metric=0\nclient_route_proto=0\n");
    assert(!accept_route_audit_result(buf));
    test_now_ms = NULL;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
}

static bool route_audit_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind != WORK_ROUTE_AUDIT) return false;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = false;
    g_dhcp_lease.route_metric = 0;
    g_dhcp_lease.route_proto = 0;
    g_client_modem_ipv4 = true;
    write_text(APP_DIR "/audit-ran", "ran\n");
    return true;
}

static void test_route_audit_worker(void)
{
    fixture_reset();
    config_defaults();
    unlink(APP_DIR "/audit-ran");
    g_fake_now_ms = 6000000;
    test_now_ms = fake_now_ms;
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_apply_pid = 0;
    g_work_pending = false;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.route_metric = 100;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_client_t1_at_ms = g_fake_now_ms + 1000;
    g_client_t2_at_ms = g_fake_now_ms + 2000;
    g_client_expiry_at_ms = g_fake_now_ms + 100000;
    test_work = route_audit_work;
    route_audit_tick();
    assert(g_apply_pid > 0 || g_work_pending);
    wait_for_path(APP_DIR "/audit-ran");
    reap_until_idle();
    assert(g_dhcp_lease.valid && g_dhcp_lease.route_ok);
    assert(!g_dhcp_lease.wifi_default);
    assert(g_dhcp_lease.route_metric == 0 && g_dhcp_lease.route_proto == 0);
    assert(g_client_t1_at_ms == g_fake_now_ms + 1000);
    assert(g_client_t2_at_ms == g_fake_now_ms + 2000);
    assert(g_client_expiry_at_ms == g_fake_now_ms + 100000);
    assert(access(APP_DIR "/audit-ran", F_OK) == 0);
    test_work = NULL;
    test_now_ms = NULL;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
}

static void test_client_status_expiry(void)
{
    char status[16384];
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), "FixtureWiFi");
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = WIFI1B_CLIENT_ROUTE_PROTO;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_client_t1_at_ms = now_ms() + 100000;
    g_client_t2_at_ms = now_ms() + 200000;
    g_client_expiry_at_ms = now_ms() + 300000;
    memset(&g_observed, 0, sizeof(g_observed));
    g_observed.gen = g_apply_gen;
    g_observed.associated = true;
    str_copy(g_observed.ip, sizeof(g_observed.ip), "10.42.0.10");
    str_copy(g_observed.ssid, sizeof(g_observed.ssid), "FixtureWiFi");
    g_observed_ms = now_ms();
    build_status(status, sizeof(status));
    assert(strstr(status, "\"client_ready\":true"));
    assert(strstr(status, "\"ip\":\"10.42.0.10\""));
    assert(strstr(status, "\"client_lease_state\":\"bound\""));

    g_dhcp_lease.route_ok = true;
    g_client_expiry_at_ms = now_ms() + 300000;
    str_copy(g_observed.ip, sizeof(g_observed.ip), "10.42.0.11");
    build_status(status, sizeof(status));
    assert(strstr(status, "\"client_ready\":false"));
    assert(strstr(status, "\"ip\":\"\""));
    str_copy(g_observed.ip, sizeof(g_observed.ip), "10.42.0.10");

    g_dhcp_lease.route_ok = false;
    build_status(status, sizeof(status));
    assert(strstr(status, "\"client_ready\":false"));
    assert(strstr(status, "\"ip\":\"\""));

    g_dhcp_lease.route_ok = true;
    g_client_expiry_at_ms = now_ms() - 1;
    build_status(status, sizeof(status));
    assert(strstr(status, "\"client_ready\":false"));
    assert(strstr(status, "\"ip\":\"\""));
    assert(strstr(status, "\"client_lease_state\":\"expired\""));
    assert(strstr(status, "\"client_lease_valid\":false"));
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
}

static void test_lease_pool_bounds(void)
{
    uint32_t first = 0, last = 0;
    uint8_t mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x55};
    fixture_reset();
    assert(w2_dhcp_pool_bounds(&first, &last));
    assert(first == w2_ip4("10.42.0.10") && last == w2_ip4("10.42.0.200"));
    /* A request outside the configured pool must never be allocated. */
    uint32_t ip = w2_lease_alloc(mac, w2_ip4("10.42.0.250"), now_ms());
    assert(ip >= first && ip <= last);
    assert(ip != w2_ip4("10.42.0.250"));
    memset(g_w2_leases, 0, sizeof(g_w2_leases));
    test_dhcp_pool_first = 200;
    test_dhcp_pool_last = 10;
    assert(!w2_dhcp_pool_bounds(&first, &last));
    assert(w2_lease_alloc(mac, 0, now_ms()) == 0);
    test_dhcp_pool_first = 0;
    test_dhcp_pool_last = 300;
    assert(!w2_dhcp_pool_bounds(&first, &last));
    test_dhcp_pool_first = -1;
    test_dhcp_pool_last = -1;
    memset(g_w2_leases, 0, sizeof(g_w2_leases));
}

static void test_release_cleanup(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n"
             "default via 192.0.2.2 dev modem0 metric 100\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_cmd_count = 0;
    assert(w2_dhcp_client_renew(W2_DHCP_RELEASE));
    assert(!g_dhcp_lease.valid && !g_dhcp_lease.route_ok);
    assert(g_dhcp_lease.updated);
    assert(command_seen("route del default via 10.42.0.1 dev wlan0 metric 101 proto 66"));
    assert(command_seen("addr del 10.42.0.10/24 dev wlan0"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
    assert(!command_seen("192.0.2.2"));

    /* A release whose address deletion is not confirmed must not be reported
     * as complete, and must retain the exact address for a retry. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = false;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    g_fail_addr_del = true;
    assert(!w2_dhcp_client_renew(W2_DHCP_RELEASE));
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));

    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_fail_addr_query = true;
    assert(!w2_dhcp_client_renew(W2_DHCP_RELEASE));
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));

    /* Route-only pending: no owned IP, but a remaining exact ledger still
     * blocks completion and keeps the interface for the next retry. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_fail_route_del = true;
    assert(!w2_dhcp_client_renew(W2_DHCP_RELEASE));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}


static size_t build_test_ack(struct w2_dhcp_packet *rep, bool with_router)
{
    memset(rep, 0, sizeof(*rep));
    rep->op = 2; rep->htype = 1; rep->hlen = 6;
    rep->xid = 0x1234;
    rep->yiaddr = htonl(w2_ip4("10.42.0.20"));
    rep->options[0] = 99; rep->options[1] = 130;
    rep->options[2] = 83; rep->options[3] = 99;
    size_t off = 4;
    uint8_t type = 5;
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 53, 1, &type);
    uint32_t server = htonl(w2_ip4("10.42.0.1"));
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 54, 4, &server);
    uint32_t mask = htonl(0xffffff00U);
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 1, 4, &mask);
    if (with_router)
        off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 3, 4, &server);
    uint32_t lease = htonl(3600);
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 51, 4, &lease);
    rep->options[off++] = 255;
    return offsetof(struct w2_dhcp_packet, options) + off;
}

static size_t build_test_ack_ext(struct w2_dhcp_packet *rep,
                                 bool with_router, uint32_t router,
                                 bool with_server, uint32_t server)
{
    memset(rep, 0, sizeof(*rep));
    rep->op = 2; rep->htype = 1; rep->hlen = 6;
    rep->xid = 0x4321;
    rep->yiaddr = htonl(w2_ip4("10.42.0.20"));
    rep->options[0] = 99; rep->options[1] = 130;
    rep->options[2] = 83; rep->options[3] = 99;
    size_t off = 4;
    uint8_t type = 5;
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 53, 1, &type);
    if (with_server) {
        uint32_t s = htonl(server);
        off += w2_dhcp_put(rep->options + off,
                           sizeof(rep->options) - off, 54, 4, &s);
    }
    uint32_t mask = htonl(0xffffff00U);
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 1, 4, &mask);
    if (with_router) {
        uint32_t r = htonl(router);
        off += w2_dhcp_put(rep->options + off,
                           sizeof(rep->options) - off, 3, 4, &r);
    }
    uint32_t lease = htonl(3600);
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off, 51, 4, &lease);
    rep->options[off++] = 255;
    return offsetof(struct w2_dhcp_packet, options) + off;
}

static size_t build_test_ack_addr(struct w2_dhcp_packet *rep, uint32_t ip,
                                  uint32_t mask, uint32_t router,
                                  bool with_router)
{
    memset(rep, 0, sizeof(*rep));
    rep->op = 2; rep->htype = 1; rep->hlen = 6;
    rep->xid = 0x5151;
    rep->yiaddr = htonl(ip);
    rep->options[0] = 99; rep->options[1] = 130;
    rep->options[2] = 83; rep->options[3] = 99;
    size_t off = 4;
    uint8_t type = 5;
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off,
                       53, 1, &type);
    uint32_t server = htonl(w2_ip4("10.42.0.1"));
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off,
                       54, 4, &server);
    uint32_t m = htonl(mask);
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off,
                       1, 4, &m);
    if (with_router) {
        uint32_t r = htonl(router);
        off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off,
                           3, 4, &r);
    }
    uint32_t lease = htonl(3600);
    off += w2_dhcp_put(rep->options + off, sizeof(rep->options) - off,
                       51, 4, &lease);
    rep->options[off++] = 255;
    return offsetof(struct w2_dhcp_packet, options) + off;
}

static void test_ack_address_validation(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];
    const uint32_t bad_ips[] = {
        w2_ip4("127.0.0.1"), 0xffffffffU, w2_ip4("224.0.0.1"),
        w2_ip4("240.0.0.1"), w2_ip4("10.42.0.0"), w2_ip4("10.42.0.255")
    };
    const uint32_t bad_masks[] = {
        0xff000000U, 0xffffffffU, 0xffffff00U,
        0xffffff00U, 0xffffff00U, 0xffffff00U
    };
    for (size_t i = 0; i < sizeof(bad_ips)/sizeof(bad_ips[0]); i++) {
        fixture_reset();
        unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
        unlink(WIFI1B_DHCP_OWNER_JOURNAL);
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_addr_state[0] = 0;
        size_t n = build_test_ack_addr(&rep, bad_ips[i], bad_masks[i],
                                       w2_ip4("10.42.0.1"), true);
        g_cmd_count = 0;
        assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                                  out_ip, sizeof(out_ip)));
        assert(!command_seen("addr replace"));
        assert(!command_seen("addr add"));
        assert(!command_seen("route add default"));
        assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
        assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
        assert(!g_dhcp_lease.valid);
    }

    /* ACQUIRE must use the negotiated OFFER/requested address. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    size_t n = build_test_ack_addr(&rep, w2_ip4("10.42.0.21"),
                                   0xffffff00U, w2_ip4("10.42.0.1"), true);
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack_ex(&rep, n, "wlan0",
                                 w2_ip4("10.42.0.20"), 0,
                                 out_ip, sizeof(out_ip), true));
    assert(!command_seen("addr replace"));
    assert(!command_seen("route add default"));

    /* Router must be on the lease subnet. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"), 0xffffff00U,
                            w2_ip4("10.0.2.2"), true);
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace"));
    assert(!command_seen("route add default"));

    /* RENEW with ACK yiaddr=0 may use only the exact existing owner. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    n = build_test_ack_addr(&rep, 0, 0xffffff00U,
                            w2_ip4("10.42.0.1"), true);
    assert(w2_dhcp_apply_ack_ex(&rep, n, "wlan0", w2_ip4("10.42.0.20"),
                                0, out_ip, sizeof(out_ip), false));
    assert(g_dhcp_lease.valid && g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);

    /* A renewal ACK changing the address is rejected. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    n = build_test_ack_addr(&rep, w2_ip4("10.42.0.21"), 0xffffff00U,
                            w2_ip4("10.42.0.1"), true);
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack_ex(&rep, n, "wlan0", 0, 0,
                                 out_ip, sizeof(out_ip), false));
    assert(!command_seen("addr replace"));

    /* An invalid ACK with a previously owned state preserves it and does
     * not wedge the exact cleanup path. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.10"), 24, "wlan0"));
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n");
    n = build_test_ack_addr(&rep, w2_ip4("127.0.0.1"), 0xff000000U,
                            w2_ip4("10.42.0.1"), true);
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace"));
    assert(!command_seen("addr del 10.42.0.10"));
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == 0);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_apply_ack_rollback(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];

    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_fail_route_add = true;
    size_t n = build_test_ack(&rep, true);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0, out_ip, sizeof(out_ip)));
    assert(command_seen("addr replace 10.42.0.20/24"));
    assert(command_seen("addr del 10.42.0.20/24"));
    assert(g_addr_state[0] == 0);
    assert(!g_dhcp_lease.valid && !g_dhcp_lease.route_ok);

    /* A failed rollback retains the exact new owner and asks the parent to
     * retry cleanup rather than forgetting it. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_fail_route_add = true;
    g_fail_addr_del = true;
    n = build_test_ack(&rep, true);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0, out_ip, sizeof(out_ip)));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_dhcp_lease.prefix == 24);
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));
    assert(strstr(g_addr_state, "10.42.0.20") != NULL);

    g_apply_gen = 71;
    g_work_kind = WORK_DHCP;
    char result[] =
        "71 0\nrollback failed\n"
        "client_lease_updated=0\nclient_lease_valid=0\n"
        "client_lease_cleanup_pending=1\n"
        "client_lease_ip=10.42.0.20\nclient_lease_server=\n"
        "client_lease_gateway=10.42.0.1\nclient_lease_prefix=24\n"
        "client_lease_lease_s=3600\nclient_lease_t1_s=1800\n"
        "client_lease_t2_s=3150\nclient_lease_route_ok=0\n"
        "client_route_metric=0\nclient_route_proto=0\n"
        "client_lease_iface=wlan0\n";
    assert(accept_apply_result(result));
    assert(!g_dhcp_lease.valid && g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_client_expiry_at_ms == 0);

    /* The old owned default route must be removed before the new address is
     * touched. A failed removal retains the old exact owner as pending. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_fail_route_del = true;
    n = build_test_ack(&rep, true);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0, out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(!command_seen("addr del 10.42.0.20/24"));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);

    /* Missing router fails before touching the address or old route. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    n = build_test_ack(&rep, false);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0, out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(!command_seen("route del default"));

    /* A failed old-address removal preserves the previous exact owner. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    g_fail_addr_del = true;
    n = build_test_ack(&rep, true);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0, out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    g_fail_addr_del = false;

    /* Route add succeeded but readback/delete failed: the exact route
     * ledger survives and no ACK rollback may clear pending. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    g_route_query_fail_after = 2; /* legacy check + metric query succeed, install readback fails */
    n = build_test_ack(&rep, true);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0, out_ip, sizeof(out_ip)));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    g_route_query_fail_after = -1;
    g_fail_route_del = true;
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(g_dhcp_lease.cleanup_pending);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
    g_fail_route_del = false;
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
    assert(strstr(g_route_defaults, "192.0.2.2") != NULL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}


static void wait_for_path(const char *path)
{
    struct timespec ts = {0, 2000000};
    for (int i = 0; i < 5000; i++) {
        if (access(path, F_OK) == 0) return;
        nanosleep(&ts, NULL);
    }
    assert(0 && "timed out waiting for worker marker");
}

static void reap_until_idle(void)
{
    struct timespec ts = {0, 2000000};
    int64_t deadline = now_ms() + 5000;
    while (g_apply_pid > 0 && now_ms() < deadline) {
        apply_reap();
        if (g_apply_pid > 0) nanosleep(&ts, NULL);
    }
    assert(g_apply_pid == 0);
}

static bool blocking_apply_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind != WORK_DHCP) return true;
    /* The new address/route are applied before the child is superseded. */
    g_dhcp_lease.valid = true;
    g_dhcp_lease.updated = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.cleanup_pending = false;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.lease_s = 3600;
    g_dhcp_lease.t1_s = 1800;
    g_dhcp_lease.t2_s = 3150;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    write_text(APP_DIR "/cancel-ready", "ready\n");
    char go[512];
    snprintf(go, sizeof(go), "%s/cancel-go", APP_DIR);
    struct timespec ts = {0, 2000000};
    for (int i = 0; i < 5000 && access(go, F_OK) != 0; i++)
        nanosleep(&ts, NULL);
    return true;
}

static void test_pending_result_precedence(void)
{
    g_apply_gen = 81;
    g_work_kind = WORK_DHCP;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    char both[] =
        "81 1\nmode off after apply\n"
        "client_lease_updated=1\nclient_lease_valid=0\n"
        "client_lease_cleanup_pending=1\n"
        "client_lease_ip=10.42.0.20\nclient_lease_prefix=24\n"
        "client_lease_gateway=10.42.0.1\n"
        "client_lease_route_ok=0\nclient_lease_iface=wlan0\n";
    assert(accept_apply_result(both));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_client_expiry_at_ms == 0);

    /* Route-only pending (IP already absent) still adopts the interface. */
    g_apply_gen = 82;
    char route_only[] =
        "82 0\nold route remains\n"
        "client_lease_updated=1\nclient_lease_valid=0\n"
        "client_lease_cleanup_pending=1\n"
        "client_lease_ip=\nclient_lease_prefix=0\n"
        "client_lease_route_ok=0\nclient_lease_iface=wlan0\n";
    assert(accept_apply_result(route_only));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));

    /* A malformed pending result must not discard the existing exact owner. */
    g_apply_gen = 83;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.30");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    char malformed[] =
        "83 0\npending but no iface\n"
        "client_lease_updated=1\nclient_lease_valid=0\n"
        "client_lease_cleanup_pending=1\n"
        "client_lease_ip=\nclient_lease_prefix=0\n";
    assert(accept_apply_result(malformed));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.30"));
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_off_mode_cleanup_retry(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\\n");
    test_work = cleanup_test_work;
    g_apply_pid = 0;
    g_work_pending = false;
    g_dhcp_work_step = W2_DHCP_RENEW;
    dhcp_lease_tick();
    assert(g_apply_pid > 0);
    assert(g_work_kind == WORK_DHCP);
    assert(g_dhcp_work_step == W2_DHCP_RELEASE);
    reap_until_idle();
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.updated);
    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.ip == 0);

    /* CarPlay (a non-client active mode) must also retry pending cleanup. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    g_apply_pid = 0;
    g_work_pending = false;
    g_dhcp_work_step = W2_DHCP_RENEW;
    dhcp_lease_tick();
    assert(g_apply_pid > 0);
    assert(g_work_kind == WORK_DHCP);
    assert(g_dhcp_work_step == W2_DHCP_RELEASE);
    reap_until_idle();
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);
    test_work = NULL;
}


static bool successful_apply_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind != WORK_DHCP) return true;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.updated = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.server = w2_ip4("10.42.0.1");
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.lease_s = 3600;
    g_dhcp_lease.t1_s = 1800;
    g_dhcp_lease.t2_s = 3150;
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = 66;
    g_client_modem_ipv4 = true;
    g_client_modem_unknown = false;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    return true;
}

static void test_current_dhcp_result_accepted(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_dhcp_work_step = W2_DHCP_ACQUIRE;
    test_work = successful_apply_work;
    g_apply_pid = 0;
    g_work_pending = false;
    request_work(WORK_DHCP, false);
    assert(g_apply_pid > 0);
    reap_until_idle();
    assert(g_dhcp_lease.valid);
    assert(g_dhcp_lease.route_ok);
    assert(g_dhcp_lease.updated);
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_client_expiry_at_ms > 0);
    test_work = NULL;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
}

static void test_superseded_dhcp_result(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_dhcp_work_step = W2_DHCP_ACQUIRE;
    unlink(APP_DIR "/cancel-ready");
    unlink(APP_DIR "/cancel-go");
    char ready[512], go[512];
    snprintf(ready, sizeof(ready), "%s/cancel-ready", APP_DIR);
    snprintf(go, sizeof(go), "%s/cancel-go", APP_DIR);
    test_work = blocking_apply_work;
    g_apply_pid = 0;
    g_work_pending = false;
    request_work(WORK_DHCP, false);
    assert(g_apply_pid > 0);
    pid_t child = g_apply_pid;
    wait_for_path(ready);
    assert(kill(child, 0) == 0);

    /* A superseding request must not SIGTERM a DHCP worker that may already
     * have applied an address/route. */
    request_work(WORK_SCAN, false);
    assert(kill(child, 0) == 0);
    assert(g_apply_cancelled);

    write_text(go, "go\n");
    reap_until_idle();

    /* The cancelled worker's exact applied lease is harvested only for
     * cleanup; it is never exposed as a ready lease. */
    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));
    assert(g_client_expiry_at_ms == 0);

    /* The superseding scan is allowed to proceed afterwards. */
    assert(g_apply_gen > 0);
    test_work = NULL;
    unlink(ready);
    unlink(go);
    g_work_pending = false;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_pending_valid_transition(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_apply_gen = 501;
    g_work_kind = WORK_DHCP;
    char valid[] =
        "501 1\nvalid after pending\n"
        "client_lease_updated=1\nclient_lease_valid=1\n"
        "client_lease_cleanup_pending=0\n"
        "client_lease_ip=10.42.0.20\nclient_lease_server=10.42.0.1\n"
        "client_lease_gateway=10.42.0.1\nclient_lease_prefix=24\n"
        "client_lease_lease_s=3600\nclient_lease_t1_s=1800\n"
        "client_lease_t2_s=3150\nclient_lease_route_ok=1\n"
        "client_route_metric=101\nclient_route_proto=66\n"
        "client_lease_iface=wlan0\n";
    assert(accept_apply_result(valid));
    assert(g_dhcp_lease.valid);
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.20"),
                                 24, "wlan0"));
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n");

    /* A valid confirmed owner must not be released by the next tick. */
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_apply_pid = 0;
    g_work_pending = false;
    test_work = NULL;
    g_dhcp_work_step = W2_DHCP_RENEW;
    dhcp_lease_tick();
    assert(g_apply_pid == 0);
    assert(!g_work_pending);
    assert(g_dhcp_lease.valid);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
}

static void test_router_consistency_matrix(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];
    struct { uint32_t router; const char *why; } bad24[] = {
        {0, "24:network"}, {0, "24:broadcast"},
        {0, "24:self"}, {0, "24:offsubnet"}
    };
    bad24[0].router = w2_ip4("10.42.0.0");
    bad24[1].router = w2_ip4("10.42.0.255");
    bad24[2].router = w2_ip4("10.42.0.20");
    bad24[3].router = w2_ip4("10.0.2.2");
    for (size_t i = 0; i < sizeof(bad24)/sizeof(bad24[0]); i++) {
        fixture_reset();
        unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
        unlink(WIFI1B_DHCP_OWNER_JOURNAL);
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_addr_state[0] = g_route_defaults[0] = g_connected_state[0] = 0;
        size_t n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"),
                                       0xffffff00U, bad24[i].router, true);
        g_cmd_count = 0;
        assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                                  out_ip, sizeof(out_ip)));
        assert(!command_seen("addr replace"));
        assert(!command_seen("addr add"));
        assert(!command_seen("route add default"));
        assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
        assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
        assert(!g_dhcp_lease.valid);
    }

    /* /31 paired peer is accepted; self and off-subnet are refused. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 10.42.0.21 dev wlan0 proto 66 metric 1000\n");
    size_t n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"),
                                   0xfffffffeU, w2_ip4("10.42.0.21"), true);
    g_cmd_count = 0;
    assert(w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                             out_ip, sizeof(out_ip)));
    assert(g_dhcp_lease.valid && g_dhcp_lease.gateway == w2_ip4("10.42.0.21"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.21 dev wlan0 proto 66 metric 1000\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);

    for (int i = 0; i < 2; i++) {
        uint32_t router = i == 0 ? w2_ip4("10.42.0.20") : w2_ip4("10.0.2.2");
        fixture_reset();
        unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
        unlink(WIFI1B_DHCP_OWNER_JOURNAL);
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_addr_state[0] = g_route_defaults[0] = g_connected_state[0] = 0;
        n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"),
                                0xfffffffeU, router, true);
        g_cmd_count = 0;
        assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                                  out_ip, sizeof(out_ip)));
        assert(!command_seen("addr replace"));
        assert(!command_seen("route add default"));
        assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
    }

    /* /32 routed leases have no separately verified on-link proof. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"), 0xffffffffU,
                            w2_ip4("10.42.0.21"), true);
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace"));
    assert(!command_seen("route add default"));
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"), 0xffffffffU,
                            w2_ip4("10.42.0.20"), true);
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));

    /* Route writer must enforce the same subnet predicate when a journal
     * owner is available. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.20"), 24, "wlan0"));
    assert(!w2_route_ledger_publish("10.42.0.255", "wlan0", 1000, 66));
    assert(!w2_route_ledger_publish("10.42.0.20", "wlan0", 1000, 66));
    assert(!w2_route_ledger_publish("10.0.2.2", "wlan0", 1000, 66));
    assert(w2_route_ledger_publish("10.42.0.1", "wlan0", 1000, 66));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);

    /* An invalid router with a previously owned lease preserves it and its
     * cleanup path remains usable. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 101;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.10"), 24, "wlan0"));
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n");
    n = build_test_ack_addr(&rep, w2_ip4("10.42.0.20"), 0xffffff00U,
                            w2_ip4("10.42.0.0"), true);
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace"));
    assert(!command_seen("addr del 10.42.0.10"));
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_router_option_strict(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];

    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    size_t n = build_test_ack_ext(&rep, false, 0, true, w2_ip4("10.42.0.1"));
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, w2_ip4("10.42.0.1"),
                              out_ip, sizeof(out_ip)));
    assert(!g_dhcp_lease.valid);
    assert(!command_seen("addr replace"));
    assert(!command_seen("route add default"));
    assert(strstr(g_dhcp_error, "router option 3") != NULL);

    /* Option 3 is the gateway; the server identifier remains distinct. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 10.42.0.2 dev wlan0 proto 66 metric 1000\n");
    n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.2"),
                           false, w2_ip4("10.42.0.1"));
    assert(w2_dhcp_apply_ack(&rep, n, "wlan0", 0, w2_ip4("10.42.0.1"),
                             out_ip, sizeof(out_ip)));
    assert(g_dhcp_lease.valid);
    assert(g_dhcp_lease.gateway == w2_ip4("10.42.0.2"));
    assert(g_dhcp_lease.server == w2_ip4("10.42.0.1"));
    assert(command_seen("route add default via 10.42.0.2"));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_route_intent_and_rollback(void)
{
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    assert(mkdir(WIFI1B_CLIENT_ROUTE_LEDGER, 0700) == 0);
    g_cmd_count = 0;
    assert(!w2_owned_route_install("10.42.0.1", "wlan0", 1000, 66));
    assert(!command_seen("route add default"));
    rmdir(WIFI1B_CLIENT_ROUTE_LEDGER);

    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    g_fail_route_query = true;
    assert(!w2_owned_route_install("10.42.0.1", "wlan0", 1000, 66));
    char ledger[256] = "";
    assert(read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0);
    assert(strstr(ledger, "scope=wifi1b-dhcp-v1") != NULL);
    assert(strstr(ledger, "gw=10.42.0.1") != NULL);
    assert(strstr(ledger, "metric=1000") != NULL);
    assert(strstr(ledger, "proto=66") != NULL);

    g_fail_route_query = false;
    g_fail_route_del = true;
    assert(!w2_owned_route_cleanup());
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);

    g_fail_route_del = false;
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_owned_route_cleanup());
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
}

static void test_journal_intent_written_before_effect(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    assert(mkdir(WIFI1B_DHCP_OWNER_JOURNAL, 0700) == 0);
    g_cmd_count = 0;
    size_t n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.1"),
                                 true, w2_ip4("10.42.0.1"));
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace"));
    assert(!command_seen("addr add"));
    assert(!command_seen("route add default"));
    assert(!g_dhcp_lease.valid);
    assert(!w2_dhcp_cleanup_owned("wlan0", true)); /* malformed journal */
    rmdir(WIFI1B_DHCP_OWNER_JOURNAL);
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
}

static void test_no_result_after_effect(void)
{
    fixture_reset();
    unlink(APPLY_RESULT);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_apply_gen = 601;
    g_work_kind = WORK_DHCP;
    g_work_pending = false;
    g_apply_cancelled = false;
    g_apply_dhcp_capable = true;
    g_txn_active = true;
    g_apply_deadline_ms = now_ms() + 60000;
    g_apply_pid = 0;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        (void)w2_dhcp_journal_write("pending", w2_ip4("10.42.0.20"),
                                    24, "wlan0");
        (void)w2_route_ledger_publish("10.42.0.1", "wlan0", 1000, 66);
        _exit(0);
    }
    g_apply_pid = pid;
    reap_until_idle();
    assert(!g_dhcp_lease.valid);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);

    g_txn_active = false;
    g_apply_dhcp_capable = false;
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == 0);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_transaction_deadline(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];

    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    size_t n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.1"),
                                 true, w2_ip4("10.42.0.1"));
    g_fake_now_ms = 1000000;
    test_now_ms = fake_now_ms;
    g_txn_active = true;
    g_apply_dhcp_capable = true;
    g_apply_deadline_ms = g_fake_now_ms + 60000;
    g_fake_now_ms += 61000; /* beyond the old 35 s assumption */
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(strstr(g_dhcp_error, "deadline") != NULL);
    assert(!command_seen("addr replace"));
    assert(!command_seen("route add default"));
    /* Every command is capped by the remaining transaction budget. */
    g_cmd_count = 0;
    assert(sh("true") == 124);
    assert(g_cmd_count == 0);
    test_now_ms = NULL;
    g_txn_active = false;
    g_apply_dhcp_capable = false;

    /* A deadline reached after the address mutation leaves the exact
     * journal owner pending; it is not forgotten or broadly flushed. */
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.1"),
                           true, w2_ip4("10.42.0.1"));
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    g_fake_now_ms = 2000000;
    test_now_ms = fake_now_ms;
    g_txn_active = true;
    g_apply_dhcp_capable = true;
    g_apply_deadline_ms = g_fake_now_ms + 60000;
    g_advance_on_addr_apply = 61000;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    test_now_ms = NULL;
    g_txn_active = false;
    g_apply_dhcp_capable = false;

    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == 0);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static bool deadline_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind != WORK_DHCP) return true;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.updated = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.lease_s = 3600;
    g_dhcp_lease.t1_s = 1800;
    g_dhcp_lease.t2_s = 3150;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    (void)w2_dhcp_journal_write("pending", g_dhcp_lease.ip, 24, "wlan0");
    (void)w2_route_ledger_publish("10.42.0.1", "wlan0", 1000, 66);
    write_text(APP_DIR "/deadline-effect", "done\n");
    char go[512];
    snprintf(go, sizeof(go), "%s/deadline-go", APP_DIR);
    struct timespec ts = {0, 2000000};
    for (int i = 0; i < 5000 && access(go, F_OK) != 0; i++)
        nanosleep(&ts, NULL);
    return true;
}

static void test_worker_deadline_harvest(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_dhcp_work_step = W2_DHCP_ACQUIRE;
    g_fake_now_ms = 3000000;
    test_now_ms = fake_now_ms;
    test_work = deadline_work;
    g_apply_pid = 0;
    g_work_pending = false;
    g_apply_cancelled = false;
    request_work(WORK_DHCP, false);
    assert(g_apply_pid > 0);
    wait_for_path(APP_DIR "/deadline-effect");
    g_fake_now_ms += 61000; /* parent-observed total worker time */
    reap_until_idle();

    /* The completed worker ran past the transaction deadline: its exact
     * effects are harvested for cleanup, never published as ready. */
    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);

    test_now_ms = NULL;
    test_work = NULL;
    unlink(APP_DIR "/deadline-effect");
    unlink(APP_DIR "/deadline-go");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == 0);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static bool blocking_mode_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind != WORK_MODE) return true;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.updated = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.lease_s = 3600;
    g_dhcp_lease.t1_s = 1800;
    g_dhcp_lease.t2_s = 3150;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    write_text(APP_DIR "/mode-ready", "ready\n");
    char go[512];
    snprintf(go, sizeof(go), "%s/mode-go", APP_DIR);
    struct timespec ts = {0, 2000000};
    for (int i = 0; i < 5000 && access(go, F_OK) != 0; i++)
        nanosleep(&ts, NULL);
    return true;
}

static void test_superseded_mode_client(void)
{
    fixture_reset();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    unlink(APP_DIR "/mode-ready");
    unlink(APP_DIR "/mode-go");
    char ready[512], go[512];
    snprintf(ready, sizeof(ready), "%s/mode-ready", APP_DIR);
    snprintf(go, sizeof(go), "%s/mode-go", APP_DIR);
    test_work = blocking_mode_work;
    g_apply_pid = 0;
    g_work_pending = false;
    g_apply_cancelled = false;
    request_work(WORK_MODE, false);
    assert(g_apply_pid > 0);
    int child_gen = g_apply_gen;
    pid_t child = g_apply_pid;
    wait_for_path(ready);
    assert(g_apply_dhcp_capable);
    assert(kill(child, 0) == 0);

    request_work(WORK_SCAN, false);
    assert(kill(child, 0) == 0);
    assert(g_apply_gen > child_gen);
    assert(g_apply_cancelled);
    write_text(go, "go\n");
    reap_until_idle();

    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_client_expiry_at_ms == 0);
    test_work = NULL;
    unlink(ready);
    unlink(go);
    g_work_pending = false;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_owner_numeric_parsing(void)
{
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    w2_dhcp_lineage_t lineage;
    assert(w2_dhcp_lineage_ensure(5, &lineage));
    w2_dhcp_lineage_t loaded_lineage;
    assert(w2_dhcp_lineage_load(&loaded_lineage) == 1);
    assert(loaded_lineage.high_water == lineage.high_water);
    assert(loaded_lineage.high_water >= 5);
    assert(!strcmp(loaded_lineage.token, lineage.token));

    w2_owner_journal_t j;
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.99"), 24, "wlan0"));
    assert(w2_dhcp_journal_load(&j) == 1);
    assert(j.gen == g_apply_gen && j.pid == getpid());
    assert(j.ip == w2_ip4("10.42.0.99") && j.prefix == 24);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);

    char line[512];
    const char *bad[] = {
        "state=current gen=1junk pid=2junk iface=wlan0 ip=10.42.0.99 prefix=24junk",
        "state=current gen=4294967296 pid=4294967297 iface=wlan0 ip=10.42.0.99 prefix=4294967320",
        "state=current gen=-1 pid=0 iface=wlan0 ip=10.42.0.99 prefix=0",
        "state=current gen= pid=2 iface=wlan0 ip=10.42.0.99 prefix=24",
        "state=current gen=1 pid=2 iface=wlan0 ip=10.42.0.99",
        "state=current gen=1 pid=2 iface=wlan0 ip=10.42.0.99 prefix=24 extra",
        "state=current gen=1 pid=2 iface=wlan0 ip=10.42.0.99 prefix=24junk",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        snprintf(line, sizeof(line),
                 "v2 scope=wifi1b-dhcp-v1 lineage=%s %s\n",
                 lineage.token, bad[i]);
        write_private(WIFI1B_DHCP_OWNER_JOURNAL, line);
        assert(w2_dhcp_journal_load(&j) == -1);
        snprintf(g_addr_state, sizeof(g_addr_state),
                 "3: wlan0    inet 10.42.0.99/24 brd 10.42.0.255 scope global wlan0\n");
        g_cmd_count = 0;
        assert(!w2_dhcp_cleanup_owned("wlan0", true));
        assert(!command_seen("addr del 10.42.0.99"));
        assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
        unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    }

    write_private(WIFI1B_DHCP_LINEAGE,
                  "v1 scope=wifi1b-dhcp lineage=0123456789abcdef0123456789abcdef high_water=4294967296\n");
    assert(w2_dhcp_lineage_load(&loaded_lineage) == -1);
    write_private(WIFI1B_DHCP_LINEAGE,
                  "v1 scope=wifi1b-dhcp lineage=0123456789abcdef0123456789abcdef high_water=5junk\n");
    assert(w2_dhcp_lineage_load(&loaded_lineage) == -1);
    write_private(WIFI1B_DHCP_LINEAGE,
                  "v1 scope=wifi1b-dhcp lineage=0123456789abcdef0123456789abcdef\n");
    assert(w2_dhcp_lineage_load(&loaded_lineage) == -1);

    /* Route ledger numerics use the same strict full-token parser. */
    unlink(WIFI1B_DHCP_LINEAGE);
    assert(w2_dhcp_lineage_ensure(5, &lineage));
    assert(w2_route_ledger_publish("10.42.0.1", "wlan0", 101, 66));
    w2_owned_route_t route;
    assert(w2_owned_route_load(&route) == 1);
    assert(!strcmp(route.gateway, "10.42.0.1"));
    assert(!strcmp(route.iface, "wlan0"));
    assert(route.metric == 101 && route.proto == 66);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    const char *bad_route[] = {
        "gen=1junk pid=2 gw=10.42.0.1 iface=wlan0 metric=101 proto=66",
        "gen=4294967296 pid=4294967297 gw=10.42.0.1 iface=wlan0 metric=101 proto=66",
        "gen=1 pid=2junk gw=10.42.0.1 iface=wlan0 metric=101 proto=66",
        "gen=1 pid=2 gw=10.42.0.1 iface=wlan0 metric=4294967296 proto=66",
        "gen=1 pid=2 gw=10.42.0.1 iface=wlan0 metric=101 proto=66junk",
        "gen=1 pid=2 gw=10.42.0.1 iface=wlan0 metric=101",
        "gen=1 pid=2 gw=10.42.0.1 iface=wlan0 metric=101 proto=66 extra",
    };
    for (size_t i = 0; i < sizeof(bad_route)/sizeof(bad_route[0]); i++) {
        snprintf(line, sizeof(line),
                 "v2 scope=wifi1b-dhcp-v1 lineage=%s %s\n",
                 lineage.token, bad_route[i]);
        write_private(WIFI1B_CLIENT_ROUTE_LEDGER, line);
        assert(w2_owned_route_load(&route) == -1);
        g_cmd_count = 0;
        assert(!w2_owned_route_cleanup());
        assert(!command_seen("route del default"));
        assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
        unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    }

    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_LINEAGE);
}

static void test_foreign_ledger_preserved(void)
{
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    write_text(WIFI1B_CLIENT_ROUTE_LEDGER, "v1 192.0.2.2 modem0 100 4\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.2 dev modem0 proto 4 metric 100\n");
    g_cmd_count = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    assert(!w2_owned_route_cleanup());
    assert(!command_seen("192.0.2.2"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);

    /* A product-looking trusted-mode v2 with the wrong durable lineage is
     * unknown, not authorization to delete a wlan0 route. */
    w2_dhcp_lineage_t lin;
    assert(w2_dhcp_lineage_ensure(5, &lin));
    write_private(WIFI1B_CLIENT_ROUTE_LEDGER,
                  "v2 scope=wifi1b-dhcp-v1 lineage=00000000000000000000000000000000 "
                  "gen=1 pid=1 gw=192.0.2.2 iface=wlan0 metric=100 proto=66\n");
    assert(!w2_owned_route_cleanup());
    assert(!command_seen("192.0.2.2"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
}

static void test_journal_lineage_and_contradiction(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];

    /* Syntactically valid foreign journal with no lineage: reject/preserve. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    write_text(WIFI1B_DHCP_OWNER_JOURNAL,
               "v1 state=current gen=999 pid=999 iface=wlan0 ip=10.42.0.99 prefix=24\n");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.99/24 brd 10.42.0.255 scope global wlan0\n");
    g_cmd_count = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(!command_seen("addr del 10.42.0.99"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);

    /* Malformed journal plus a captured parent: no mutation, preserve both. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    write_text(WIFI1B_DHCP_OWNER_JOURNAL,
               "v1 state=current gen=1 pid=1 iface=wlan0 ip=10.42.0.99 prefix=24 extra\n");
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n"
             "3: wlan0    inet 10.42.0.99/24 brd 10.42.0.255 scope global wlan0\n");
    size_t n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.1"),
                                  true, w2_ip4("10.42.0.1"));
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(strstr(g_addr_state, "10.42.0.99") != NULL);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);

    /* A valid committed product journal and an empty parent capture: do not
     * overwrite the journal before its exact owner is cleaned. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.99"), 24, "wlan0"));
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.99/24 brd 10.42.0.255 scope global wlan0\n");
    n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.1"),
                           true, w2_ip4("10.42.0.1"));
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(strstr(g_addr_state, "10.42.0.99") != NULL);
    w2_owner_journal_t j;
    assert(w2_dhcp_journal_load(&j) == 1);
    assert(j.ip == w2_ip4("10.42.0.99"));
    write_private(WIFI1B_DHCP_OWNER_JOURNAL,
                  "v2 scope=wifi1b-dhcp-v1 lineage=00000000000000000000000000000000 "
                  "state=current gen=1 pid=1 iface=wlan0 ip=10.42.0.99 prefix=24\n");
    assert(w2_dhcp_journal_load(&j) == -1);
    g_cmd_count = 0;
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(!command_seen("addr del 10.42.0.99"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    /* Restore the valid committed owner for the contradiction case. */
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.99"), 24, "wlan0"));

    /* A captured parent with a different exact owner is a contradiction: no
     * mutation and no journal overwrite. */
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n"
             "3: wlan0    inet 10.42.0.99/24 brd 10.42.0.255 scope global wlan0\n");
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(strstr(g_addr_state, "10.42.0.99") != NULL);
    assert(w2_dhcp_journal_load(&j) == 1 && j.ip == w2_ip4("10.42.0.99"));

    /* Negative generation/pid are never accepted as authorization. */
    w2_dhcp_lineage_t lineage;
    assert(w2_dhcp_lineage_load(&lineage) == 1);
    char line[256];
    snprintf(line, sizeof(line),
             "v2 scope=wifi1b-dhcp-v1 lineage=%s state=current gen=-1 pid=%d "
             "iface=wlan0 ip=10.42.0.99 prefix=24\n",
             lineage.token, (int)getpid());
    write_text(WIFI1B_DHCP_OWNER_JOURNAL, line);
    g_cmd_count = 0;
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(!command_seen("addr del 10.42.0.99"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    snprintf(line, sizeof(line),
             "v2 scope=wifi1b-dhcp-v1 lineage=%s state=current gen=1 pid=0 "
             "iface=wlan0 ip=10.42.0.99 prefix=24\n",
             lineage.token);
    write_text(WIFI1B_DHCP_OWNER_JOURNAL, line);
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(!command_seen("addr del 10.42.0.99"));

    /* Generation above the durable high-water mark is also unknown. */
    snprintf(line, sizeof(line),
             "v2 scope=wifi1b-dhcp-v1 lineage=%s state=current gen=%d pid=%d "
             "iface=wlan0 ip=10.42.0.99 prefix=24\n",
             lineage.token, lineage.high_water + 1, (int)getpid());
    write_text(WIFI1B_DHCP_OWNER_JOURNAL, line);
    g_cmd_count = 0;
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(!command_seen("addr del 10.42.0.99"));
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
}

static void test_prepublish_preserves_parent(void)
{
    struct w2_dhcp_packet rep;
    char out_ip[64];
    size_t n = build_test_ack_ext(&rep, true, w2_ip4("10.42.0.1"),
                                  true, w2_ip4("10.42.0.1"));

    /* Open/write failure before publication: the captured parent .20 is
     * retained pending and no address/route mutation is attempted. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    test_owner_write_fail = 1; /* injected open/write failure */
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    test_owner_write_fail = 0;
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(strstr(g_addr_state, "10.42.0.20") != NULL);

    /* Explicit post-write stages fail before effects and preserve parent. */
    for (int stage = 2; stage <= 3; stage++) {
        fixture_reset();
        unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
        unlink(WIFI1B_DHCP_OWNER_JOURNAL);
        assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.20"),
                                     24, "wlan0"));
        unlink(WIFI1B_DHCP_OWNER_JOURNAL);
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_dhcp_lease.valid = true;
        g_dhcp_lease.ip = w2_ip4("10.42.0.20");
        g_dhcp_lease.prefix = 24;
        str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
        snprintf(g_addr_state, sizeof(g_addr_state),
                 "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
        test_owner_write_fail = stage;
        g_cmd_count = 0;
        assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                                  out_ip, sizeof(out_ip)));
        test_owner_write_fail = 0;
        assert(!command_seen("addr replace 10.42.0.20/24"));
        assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
        assert(g_dhcp_lease.cleanup_pending);
        assert(!g_dhcp_lease.updated);
        assert(strstr(g_addr_state, "10.42.0.20") != NULL);
    }
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);

    /* Directory fsync failure after the file write is still a
     * pre-publication failure: preserve parent and report unclean. */
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.20"),
                                 24, "wlan0"));
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    test_owner_dir_sync_fail = 1;
    g_cmd_count = 0;
    assert(!w2_dhcp_apply_ack(&rep, n, "wlan0", 0, 0,
                              out_ip, sizeof(out_ip)));
    test_owner_dir_sync_fail = 0;
    assert(!command_seen("addr replace 10.42.0.20/24"));
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_owner_fs_unconfirmed);
    assert(strstr(g_addr_state, "10.42.0.20") != NULL);
    /* The published-but-unsynced pending journal is retried safely. */
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_owner_fs_unconfirmed);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
}

static void test_reconcile_requires_live_kernel(void)
{
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.20"), 24, "wlan0"));
    write_test_route_ledger("10.42.0.1", "wlan0", 1000, 66);

    /* Files match but both live objects are absent. */
    g_addr_state[0] = 0;
    g_route_defaults[0] = 0;
    assert(w2_dhcp_reconcile_owned_state());
    assert(!g_dhcp_lease.valid && !g_dhcp_lease.route_ok);

    /* Address live, route absent. */
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    g_route_defaults[0] = 0;
    assert(w2_dhcp_reconcile_owned_state());
    assert(!g_dhcp_lease.valid);

    /* Route live, address absent. */
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_addr_state[0] = 0;
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    assert(w2_dhcp_reconcile_owned_state());
    assert(!g_dhcp_lease.valid);

    /* Both live: files and kernel agree, so the ready lease survives. */
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    assert(!w2_dhcp_reconcile_owned_state());
    assert(g_dhcp_lease.valid && g_dhcp_lease.route_ok);

    /* Kernel readback failure is unclean, never false-ready. */
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_fail_route_query = true;
    assert(w2_dhcp_reconcile_owned_state());
    assert(!g_dhcp_lease.valid);
    g_fail_route_query = false;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_fail_addr_query = true;
    assert(w2_dhcp_reconcile_owned_state());
    assert(!g_dhcp_lease.valid);
    g_fail_addr_query = false;
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
}

static bool selfkill_deadline_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind != WORK_DHCP) return true;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.20");
    g_dhcp_lease.prefix = 24;
    g_dhcp_lease.gateway = w2_ip4("10.42.0.1");
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.route_metric = 1000;
    g_dhcp_lease.route_proto = 66;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    (void)w2_dhcp_journal_write("pending", g_dhcp_lease.ip, 24, "wlan0");
    (void)w2_route_ledger_publish("10.42.0.1", "wlan0", 1000, 66);
    write_text(APP_DIR "/selfkill-ready", "ready\n");
    test_command = NULL;
    int rc = sh("sleep 5");
    test_command = fixture_command;
    return rc == 0;
}

static void test_worker_selfkill_deadline(void)
{
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    unlink(APP_DIR "/selfkill-ready");
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_dhcp_work_step = W2_DHCP_ACQUIRE;
    test_work = selfkill_deadline_work;
    test_txn_budget_ms = 150;
    g_apply_pid = 0;
    g_work_pending = false;
    request_work(WORK_DHCP, false);
    assert(g_apply_pid > 0);
    wait_for_path(APP_DIR "/selfkill-ready");
    reap_until_idle();

    /* The worker killed its own command group at the short deadline; the
     * parent reconciled the durable effect and never published it ready. */
    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.20"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0);

    test_work = NULL;
    test_txn_budget_ms = 0;
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.20/24 brd 10.42.0.255 scope global wlan0\n");
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 10.42.0.1 dev wlan0 proto 66 metric 1000\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.2 dev modem0 metric 100\n");
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == 0);
    unlink(APP_DIR "/selfkill-ready");
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

static void test_deadline_command_is_killed(void)
{
    fixture_reset();
    test_command = NULL;
    g_txn_active = true;
    g_apply_dhcp_capable = true;
    g_apply_deadline_ms = now_ms() + 50;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = sh("sleep 5");
    clock_gettime(CLOCK_MONOTONIC, &end);
    int64_t elapsed = (end.tv_sec - start.tv_sec) * 1000LL +
                      (end.tv_nsec - start.tv_nsec) / 1000000LL;
    g_txn_active = false;
    g_apply_dhcp_capable = false;
    test_command = fixture_command;
    assert(rc == 124);
    assert(elapsed < 3000);
}

static void test_journal_clear_fsync_failure(void)
{
    fixture_reset();
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.10"), 24, "wlan0"));
    snprintf(g_addr_state, sizeof(g_addr_state),
             "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n");
    test_owner_dir_sync_fail = 1;
    assert(!w2_dhcp_cleanup_owned("wlan0", true));
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_owner_fs_unconfirmed);
    test_owner_dir_sync_fail = 0;
    assert(w2_dhcp_cleanup_owned("wlan0", true));
    assert(!g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_owner_fs_unconfirmed);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--route-install") && argc > 3) {
        config_defaults();
        if (radio_begin("wifi1b_daemon", true) != 1) return 1;
        bool ok = w2_client_route_install(w2_ip4(argv[2]), argv[3]);
        radio_release();
        printf("{\"ok\":%s,\"metric\":%u,\"proto\":%u,"
               "\"wifi_default\":%s,\"modem\":%s,\"legacy_removed\":%d,"
               "\"route_ok\":%s,\"error\":\"%s\"}\n",
               ok ? "true" : "false", g_dhcp_lease.route_metric,
               g_dhcp_lease.route_proto,
               g_dhcp_lease.wifi_default ? "true" : "false",
               g_client_modem_ipv4 ? "true" : "false",
               g_client_legacy_routes_removed,
               g_dhcp_lease.route_ok ? "true" : "false",
               g_dhcp_error);
        return ok ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--route-audit")) {
        config_defaults();
        char ledger[1024];
        w2_owned_route_t own;
        struct in_addr gw_addr;
        if (read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) <= 0 ||
            !w2_owned_route_parse(ledger, &own) ||
            inet_pton(AF_INET, own.gateway, &gw_addr) != 1)
            return 2;
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_dhcp_lease.valid = true;
        g_dhcp_lease.route_ok = true;
        g_dhcp_lease.wifi_default = true;
        g_dhcp_lease.ip = w2_ip4("10.42.0.10");
        g_dhcp_lease.gateway = ntohl(gw_addr.s_addr);
        g_dhcp_lease.prefix = 24;
        g_dhcp_lease.route_metric = own.metric;
        g_dhcp_lease.route_proto = own.proto;
        str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), own.iface);
        bool ok = w2_client_route_audit();
        printf("{\"ok\":%s,\"wifi_default\":%s,\"metric\":%u,"
               "\"proto\":%u,\"legacy_removed\":%d}\n",
               ok ? "true" : "false",
               g_dhcp_lease.wifi_default ? "true" : "false",
               g_dhcp_lease.route_metric, g_dhcp_lease.route_proto,
               g_client_legacy_routes_removed);
        return ok ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--route-cleanup")) {
        config_defaults();
        bool ok = w2_owned_route_cleanup();
        printf("{\"ok\":%s}\n", ok ? "true" : "false");
        return ok ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--route-metric")) {
        unsigned int metric = 0;
        test_command = NULL;
        bool ok = w2_route_select_metric(&metric);
        printf("{\"ok\":%s,\"metric\":%u}\n", ok ? "true" : "false", metric);
        return ok ? 0 : 1;
    }
    if (argc > 5 && !strcmp(argv[1], "--renew-client")) {
        config_defaults();
        test_command = NULL;
        test_mac_override = argv[5];
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_dhcp_lease.valid = true;
        g_dhcp_lease.updated = false;
        g_dhcp_lease.ip = w2_ip4(argv[3]);
        g_dhcp_lease.server = w2_ip4(argv[4]);
        g_dhcp_lease.prefix = 24;
        g_dhcp_lease.lease_s = 3600;
        g_dhcp_lease.t1_s = 1800;
        g_dhcp_lease.t2_s = 3150;
        str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
        w2_dhcp_step_t step = !strcmp(argv[2], "rebind")
            ? W2_DHCP_REBIND : W2_DHCP_RENEW;
        if (radio_begin("wifi1b_daemon", true) != 1) return 1;
        bool ok = w2_dhcp_client_renew(step);
        radio_release();
        printf("{\"ok\":%s,\"valid\":%s,\"error\":\"%s\"}\n",
               ok ? "true" : "false",
               g_dhcp_lease.valid ? "true" : "false", g_dhcp_error);
        return ok ? 0 : 1;
    }
    if (argc > 2 && !strcmp(argv[1], "--raw-client")) {
        config_defaults();
        test_command = NULL;
        test_mac_override = argv[2];
        char ip[64] = "";
        if (radio_begin("wifi1b_daemon", true) != 1) return 1;
        bool ok = w2_dhcp_client("wlan0", ip, sizeof(ip));
        radio_release();
        printf("{\"ok\":%s,\"ip\":\"%s\",\"route_ok\":%s,\"metric\":%u,"
               "\"proto\":%u,\"error\":\"%s\"}\n",
               ok ? "true" : "false", ip,
               g_dhcp_lease.route_ok ? "true" : "false",
               g_dhcp_lease.route_metric, g_dhcp_lease.route_proto,
               g_dhcp_error);
        return ok ? 0 : 1;
    }
    if (argc > 3 && !strcmp(argv[1], "--raw-recv")) {
        uint32_t xid = (uint32_t)strtoul(argv[2], NULL, 0);
        uint8_t mac[6];
        uint8_t payload[1500];
        size_t payload_len = 0;
        if (!parse_mac6(argv[3], mac)) return 2;
        int index = 0;
        int fd = w2_dhcp_raw_open("wlan0", &index);
        if (fd < 0) {
            printf("{\"ok\":false,\"error\":\"raw open\"}\n");
            return 1;
        }
        int64_t deadline = now_ms() + 5000;
        bool got = false;
        while (!got && now_ms() < deadline) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 100) > 0 &&
                (pfd.revents & POLLIN))
                got = w2_dhcp_raw_receive(fd, index, xid, mac, payload,
                                          sizeof(payload), &payload_len);
        }
        close(fd);
        printf("{\"ok\":%s,\"len\":%zu}\n", got ? "true" : "false", payload_len);
        return got ? 0 : 1;
    }

    mkdir(APP_DIR, 0700);
    test_masks_and_options();
    test_lease_action();
    test_route_policy_units();
    test_route_install_cleanup();
    test_lease_pool_bounds();
    test_release_cleanup();
    test_apply_ack_rollback();
    test_pending_result_precedence();
    test_off_mode_cleanup_retry();
    test_pending_valid_transition();
    test_owner_numeric_parsing();
    test_ack_address_validation();
    test_foreign_ledger_preserved();
    test_journal_lineage_and_contradiction();
    test_prepublish_preserves_parent();
    test_reconcile_requires_live_kernel();
    test_deadline_command_is_killed();
    test_worker_selfkill_deadline();
    test_journal_clear_fsync_failure();
    test_router_option_strict();
    test_router_consistency_matrix();
    test_route_intent_and_rollback();
    test_journal_intent_written_before_effect();
    test_no_result_after_effect();
    test_transaction_deadline();
    test_worker_deadline_harvest();
    test_current_dhcp_result_accepted();
    test_superseded_dhcp_result();
    test_superseded_mode_client();
    test_legacy_cleanup();
    test_modem_safe_route_policy();
    test_route_audit();
    test_raw_parser();
    test_accept_lease_result();
    test_route_audit_result_merge();
    test_route_audit_worker();
    test_client_status_expiry();
    puts("PASS: DHCP client identity, timers, raw parsing and owned route policy");
    return 0;
}
