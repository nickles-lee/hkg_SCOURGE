/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline and fixture-command tests for the C2 Wifi1b AP network
 * prerequisites: IPv6 link-local retention, exact IPv4/IPv6 readiness,
 * live AP readback, split configured/observed IE state, owned firewall
 * rules and ownership-safe CarPlay control.
 *
 * This file never executes a real network/firewall command: every command
 * goes through the WIFI_TEST hook. The namespace checks live in the Python
 * driver and run inside an unshared network namespace.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }

#define CMD_MAX 512
static char g_cmds[CMD_MAX][768];
static int g_cmd_count;
static const char *g_iw_fixture;
static const char *g_wpa_fixture;
static const char *g_ip4_fixture;
static const char *g_route4_fixture;
static const char *g_ifconfig_fixture;
static const char *g_iwconfig_fixture;
static const char *g_default_route_fixture;
static bool g_addr_del_effect;
static bool g_route_del_effect;
static const char *g_fail_substring;

static int fixture_command(char *out, size_t n, const char *cmd);

static void fixture_reset(void)
{
    g_cmd_count = 0;
    g_iw_fixture = NULL;
    g_wpa_fixture = NULL;
    g_ip4_fixture = NULL;
    g_route4_fixture = NULL;
    g_ifconfig_fixture = NULL;
    g_iwconfig_fixture = NULL;
    g_default_route_fixture = NULL;
    g_addr_del_effect = false;
    g_route_del_effect = false;
    g_fail_substring = NULL;
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 1;
    test_dhcpd_start = -1;
    test_dhcpd_running = -1;
    test_owner_write_fail = 0;
    test_owner_dir_sync_fail = 0;
    test_lineage_require_lock = 0;
    g_dhcp_owner_fs_unconfirmed = false;
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
}

static void write_test_route_ledger(const char *gw, const char *iface,
                                    unsigned int metric, unsigned int proto)
{
    assert(w2_route_ledger_publish(gw, iface, metric, proto));
}

static void record_command(const char *cmd)
{
    if (g_cmd_count < CMD_MAX)
        snprintf(g_cmds[g_cmd_count++], sizeof(g_cmds[0]), "%s", cmd);
}

static int fixture_command(char *out, size_t n, const char *cmd)
{
    record_command(cmd);
    if (out && n) out[0] = '\0';
    if (g_fail_substring && strstr(cmd, g_fail_substring)) return 1;
    if (strstr(cmd, " -C ")) return 1; /* force idempotent inserts */
    if (strstr(cmd, "__WIFI1B_V4__")) {
        snprintf(out, n,
                 "__WIFI1B_V4__\n"
                 "-A INPUT -i wlan0 -j WIFI1B_APP_CP\n"
                 "-A OUTPUT -o wlan0 -j WIFI1B_APP_CP\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 67 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 5353 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 5020 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 6000 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 6003 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 6011 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 5000 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 5001 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 6030 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 7000 -j ACCEPT\n"
                 "__WIFI1B_V6__\n"
                 "-A INPUT -i wlan0 -j WIFI1B_APP_CP\n"
                 "-A OUTPUT -o wlan0 -j WIFI1B_APP_CP\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 547 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 5353 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 5020 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 6000 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 6003 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p udp -m udp --dport 6011 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 5000 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 5001 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 6030 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p tcp -m tcp --dport 7000 -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -p ipv6-icmp -j ACCEPT\n"
                 "-A WIFI1B_APP_CP -d ff00::/8 -j ACCEPT\n");
        return 0;
    }
    if (strstr(cmd, "iw dev wlan0 info")) {
        if (!g_iw_fixture) return 1;
        snprintf(out, n, "%s", g_iw_fixture);
        return 0;
    }
    if (strstr(cmd, "wpa_cli") && strstr(cmd, "status")) {
        if (!g_wpa_fixture) return 1;
        snprintf(out, n, "%s", g_wpa_fixture);
        return 0;
    }
    if (strstr(cmd, "iwconfig wlan0")) {
        if (!g_iwconfig_fixture) return 1;
        snprintf(out, n, "%s", g_iwconfig_fixture);
        return 0;
    }
    if (strstr(cmd, "ip -o -4 addr show dev wlan0")) {
        if (!g_ip4_fixture) return 1;
        snprintf(out, n, "%s", g_ip4_fixture);
        return 0;
    }
    if (strstr(cmd, "ip -4 route show dev wlan0")) {
        if (!g_route4_fixture) return 1;
        snprintf(out, n, "%s", g_route4_fixture);
        return 0;
    }
    if (strstr(cmd, "ip -4 route show default")) {
        if (!g_default_route_fixture) return 1;
        snprintf(out, n, "%s", g_default_route_fixture);
        return 0;
    }
    if (strstr(cmd, "route del default") && g_route_del_effect) {
        g_default_route_fixture = ""; /* present, but empty readback */
        return 0;
    }
    if (strstr(cmd, "addr del") && g_addr_del_effect) {
        g_ip4_fixture = ""; /* readback succeeds and proves absence */
        return 0;
    }
    if (strstr(cmd, "ifconfig wlan0")) {
        if (!g_ifconfig_fixture) return 1;
        snprintf(out, n, "%s", g_ifconfig_fixture);
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

static void assert_no_command(const char *needle)
{
    for (int i = 0; i < g_cmd_count; i++) {
        if (strstr(g_cmds[i], needle)) {
            fprintf(stderr, "FAIL: unexpected command containing '%s': %s\n",
                    needle, g_cmds[i]);
            exit(1);
        }
    }
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void write_owner(const char *text)
{
    write_text(RADIO_OWNER_PATH, text);
}

static void remove_owner(void)
{
    unlink(RADIO_OWNER_PATH);
}

static const char *g_if_inet6_path;
static const char *g_ipv6_conf_dir;
static char g_if_inet6_path_buf[512];
static char g_ipv6_conf_dir_buf[512];

static void prepare_ipv6_fixture(const char *inet6_text, int disabled)
{
    snprintf(g_if_inet6_path_buf, sizeof(g_if_inet6_path_buf),
             "%s/if_inet6", APP_DIR);
    write_text(g_if_inet6_path_buf, inet6_text);
    g_if_inet6_path = g_if_inet6_path_buf;
    test_if_inet6_path = g_if_inet6_path;

    snprintf(g_ipv6_conf_dir_buf, sizeof(g_ipv6_conf_dir_buf),
             "%s/ipv6conf", APP_DIR);
    mkdir(g_ipv6_conf_dir_buf, 0700);
    const char *levels[] = {"wlan0", "all", "default"};
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/ipv6conf/%s", APP_DIR, levels[i]);
        mkdir(dir, 0700);
        char file[512];
        snprintf(file, sizeof(file), "%s/disable_ipv6", dir);
        write_text(file, disabled ? "1\n" : "0\n");
    }
    g_ipv6_conf_dir = g_ipv6_conf_dir_buf;
    test_ipv6_conf_dir = g_ipv6_conf_dir;
}

static void set_ap_fixtures(void)
{
    g_iw_fixture =
        "Interface wlan0\n"
        "\tifindex 3\n"
        "\twdev 0x1\n"
        "\tssid IONIQ5-CarPlay-B\n"
        "\ttype AP\n"
        "\twiphy 0\n"
        "\tchannel 36 (5180 MHz), width: 20 MHz, center1: 5180 MHz\n";
    g_wpa_fixture =
        "ssid=IONIQ5-CarPlay-B\n"
        "mode=AP\n"
        "key_mgmt=WPA2-PSK\n"
        "wpa_state=COMPLETED\n";
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan0\\       "
        "valid_lft forever preferred_lft forever\n";
    g_ifconfig_fixture = NULL;
    g_iwconfig_fixture = NULL;
}

static void test_linklocal_parser(void)
{
    cp_net_linklocal ll;
    const char *text =
        "00000000000000000000000000000001 01 80 10 80       lo\n"
        "fe800000000000000000000000000001 03 40 20 40    wlan0\n"
        "fe800000000000000000000000000002 03 40 20 08    wlan0\n"
        "20010db8000000000000000000000001 03 40 00 80    wlan0\n"
        "fe800000000000000000000000000009 04 40 20 80    eth9\n"
        "fe800000000000000000000000000003 03 40 20 80    wlan0\n";

    assert(cp_net_parse_if_inet6(text, "wlan0", 0, &ll));
    assert(ll.usable);
    assert(ll.any_linklocal && ll.tentative && ll.dad_failed);
    assert(ll.ifindex == 3 && ll.prefix_len == 0x40 && ll.scope == 0x20);
    assert(!strcmp(ll.address, "fe80::3"));
    assert(!strcmp(ll.scoped, "fe80::3%wlan0"));

    /* A mismatched ifindex must not accept another interface's address. */
    assert(!cp_net_parse_if_inet6(text, "wlan0", 4, &ll));

    const char *tentative_only =
        "fe800000000000000000000000000001 03 40 20 40    wlan0\n";
    assert(!cp_net_parse_if_inet6(tentative_only, "wlan0", 0, &ll));
    assert(ll.any_linklocal && ll.tentative && !ll.usable);

    const char *dad_failed_only =
        "fe800000000000000000000000000002 03 40 20 08    wlan0\n";
    assert(!cp_net_parse_if_inet6(dad_failed_only, "wlan0", 0, &ll));
    assert(ll.any_linklocal && ll.dad_failed && !ll.usable);

    const char *global_only =
        "20010db8000000000000000000000001 03 40 00 80    wlan0\n";
    assert(!cp_net_parse_if_inet6(global_only, "wlan0", 0, &ll));

    /* Truncated/garbage lines are ignored, never partially accepted. */
    assert(!cp_net_parse_if_inet6("fe80 03 40 20 80 wlan0\n", "wlan0", 0, &ll));
    assert(!cp_net_parse_if_inet6("", "wlan0", 0, &ll));
}

static void test_command_parsers(void)
{
    cp_net_iw_info iw;
    cp_net_wpa_status wpa;
    char ip[64];

    assert(cp_net_parse_iw_info(
        "\tifindex 3\n"
        "\tssid IONIQ5\\x2dCarPlay\n"
        "\ttype AP\n"
        "\tchannel 36 (5180 MHz), width: 20 MHz\n", &iw));
    assert(iw.ifindex == 3 && iw.is_ap && !iw.is_managed);
    assert(iw.have_ssid && !strcmp(iw.ssid, "IONIQ5-CarPlay"));
    assert(iw.channel == 36 && iw.frequency == 5180);

    assert(cp_net_parse_wpa_status(
        "ssid=IONIQ5-CarPlay\n"
        "mode=AP\n"
        "key_mgmt=WPA2-PSK\n"
        "wpa_state=COMPLETED\n", &wpa));
    assert(wpa.ap_mode && wpa.completed && !wpa.station_mode);
    assert(!strcmp(wpa.ssid, "IONIQ5-CarPlay"));
    assert(!strcmp(wpa.key_mgmt, "WPA2-PSK"));

    assert(cp_net_parse_ip4_addr(
        "3: wlan0    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan0\n",
        "wlan0", ip, sizeof(ip)));
    assert(!strcmp(ip, "10.42.0.1"));
    assert(!cp_net_parse_ip4_addr(
        "4: wlan9    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan9\n",
        "wlan0", ip, sizeof(ip)));
    assert(cp_net_freq_to_channel(5180) == 36);
    assert(cp_net_freq_to_channel(2437) == 6);
}

static void test_probe_readiness_and_status(void)
{
    char status[16384];
    const char *usable =
        "fe800000000000000000000000000003 03 40 20 80    wlan0\n";

    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 1;
    set_ap_fixtures();
    prepare_ipv6_fixture(usable, 0);

    config_defaults();
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), "fixture-secret-passphrase");
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;

    ap_probe_t p;
    ap_probe(&p);
    assert(p.state == AP_READY);
    assert(p.role == 1 && !p.conflict);
    assert(p.ipv4_ready && !strcmp(p.ip, WIFI1B_AP_IP));
    assert(p.ipv6_ready && !strcmp(p.linklocal, "fe80::3%wlan0"));
    assert(p.ifindex == 3);
    assert(p.frequency == 5180 && p.channel == 36);
    assert(p.channel_matches_config);
    assert(!strcmp(p.security, "WPA2-PSK"));
    assert(!strcmp(p.ssid, "IONIQ5-CarPlay-B"));
    assert(strstr(p.security, "fixture-secret-passphrase") == NULL);
    assert(strstr(p.linklocal, "fixture-secret-passphrase") == NULL);
    assert(strstr(p.ssid, "fixture-secret-passphrase") == NULL);

    /* A stale configured channel must not replace the observed 5 GHz one. */
    g_cfg.cp_channel = 149;
    ap_probe(&p);
    assert(p.frequency == 5180 && p.channel == 36);
    assert(!p.channel_matches_config);
    assert(p.state == AP_READY); /* actual channel is known and valid */
    g_cfg.cp_channel = 36;

    /* wpa and iw disagreeing about frequency is a conflict, not readiness. */
    g_wpa_fixture =
        "ssid=IONIQ5-CarPlay-B\n"
        "mode=AP\n"
        "key_mgmt=WPA2-PSK\n"
        "freq=2437\n";
    ap_probe(&p);
    assert(p.conflict && p.state != AP_READY);

    /* A non-exact IPv4 address is not AP readiness. */
    set_ap_fixtures();
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.2/24 brd 10.42.0.255 scope global wlan0\n";
    ap_probe(&p);
    assert(!p.ipv4_ready && p.state == AP_INTERFACE_UP);

    /* No readable frequency means the on-air channel cannot be verified. */
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan0\n";
    g_iw_fixture =
        "Interface wlan0\n\tifindex 3\n\tssid IONIQ5-CarPlay-B\n\ttype AP\n";
    ap_probe(&p);
    assert(p.frequency == 0 && p.channel == 0 && p.state == AP_INTERFACE_UP);

    /* Observed status readback, with split configured vs observed IE. */
    set_ap_fixtures();
    ap_probe(&p);
    assert(p.state == AP_READY);
    memset(&g_observed, 0, sizeof(g_observed));
    g_observed.gen = g_apply_gen;
    g_observed.ap = p;
    g_observed.ap_on = p.state == AP_READY;
    str_copy(g_observed.ap_ssid, sizeof(g_observed.ap_ssid), p.ssid);
    g_observed_ms = now_ms();
    g_carplay_ie = true;
    build_status(status, sizeof(status));
    assert(strstr(status, "\"ap_ssid\":\"IONIQ5-CarPlay-B\""));
    assert(strstr(status, "\"ap_channel\":36"));
    assert(strstr(status, "\"ap_frequency\":5180"));
    assert(strstr(status, "\"ap_security\":\"WPA2-PSK\""));
    assert(strstr(status, "\"ap_linklocal\":\"fe80::3%wlan0\""));
    assert(strstr(status, "\"ap_ifindex\":3"));
    assert(strstr(status, "\"ap_ipv4_ready\":true"));
    assert(strstr(status, "\"ap_ipv6_ready\":true"));
    assert(strstr(status, "\"ap_network_ready\":true"));
    assert(strstr(status, "\"carplay_ie_configured\":true"));
    assert(strstr(status, "\"carplay_ie_observed\":false"));
    assert(strstr(status, "\"carplay_ie_observation\":\"ie_unverified\""));
    assert(strstr(status, "\"ap_security\":\"fixture-secret-passphrase\"") == NULL);
}

static void test_ipv6_failures(void)
{
    ap_probe_t p;
    const char *tentative =
        "fe800000000000000000000000000001 03 40 20 40    wlan0\n";
    const char *dad_failed =
        "fe800000000000000000000000000002 03 40 20 08    wlan0\n";
    const char *global_only =
        "20010db8000000000000000000000001 03 40 00 80    wlan0\n";

    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 1;
    set_ap_fixtures();
    config_defaults();
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), "fixture-secret-passphrase");
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;

    prepare_ipv6_fixture(tentative, 0);
    ap_probe(&p);
    assert(!p.ipv6_ready && !p.ipv6_unavailable);
    assert(strstr(p.ipv6_error, "tentative") != NULL);
    assert(p.state == AP_INTERFACE_UP);
    assert(ap_wait_ready(false, &p) == AP_WAIT_PENDING);

    prepare_ipv6_fixture(dad_failed, 0);
    ap_probe(&p);
    assert(!p.ipv6_ready && p.ipv6_unavailable);
    assert(strstr(p.ipv6_error, "DAD failed") != NULL);
    assert(ap_wait_ready(false, &p) == AP_WAIT_IPV6_BLOCKED);

    prepare_ipv6_fixture(global_only, 1);
    ap_probe(&p);
    assert(!p.ipv6_ready && p.ipv6_unavailable);
    assert(strstr(p.ipv6_error, "disable_ipv6=1") != NULL);
    assert(ap_wait_ready(false, &p) == AP_WAIT_IPV6_BLOCKED);
    /* Diagnosis is read-only: the fixture still says disabled. */
    char value[32];
    assert(w2_ipv6_conf_read("wlan0", "disable_ipv6", value, sizeof(value)));
    assert(!strcmp(trim(value), "1"));
}

static void test_firewall_rules(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();

    assert(ap_fw6_open());
    assert(command_seen("-N WIFI1B_APP_AP6"));
    assert(command_seen("-C INPUT -i wlan0 -j WIFI1B_APP_AP6"));
    assert(command_seen("-I INPUT -i wlan0 -j WIFI1B_APP_AP6"));
    assert(command_seen("-p ipv6-icmp -j ACCEPT"));
    assert(command_seen("-d ff00::/8 -j ACCEPT"));
    assert(command_seen("--dport 547"));
    assert_no_command("-I INPUT -j ");
    assert_no_command("-A INPUT ");

    g_cmd_count = 0;
    assert(carplay_fw_open());
    assert(command_seen("-p udp --dport 5020 -j ACCEPT"));
    assert(command_seen("-p tcp --dport 6030 -j ACCEPT"));
    assert(command_seen("-p udp --dport 5353 -j ACCEPT"));
    assert(command_seen("-p tcp --dport 7000 -j ACCEPT"));
    assert(command_seen("-p ipv6-icmp -j ACCEPT"));
    assert(command_seen("-d ff00::/8 -j ACCEPT"));
    assert_no_command("-I INPUT -j ");
    assert_no_command("-I FORWARD");
    assert_no_command(" -A INPUT ");

    /* IPv4 AP chain keeps its existing blanket accept only inside the
     * owned interface chain; no new interface/global hook is added. */
    g_cmd_count = 0;
    assert(ap_fw_open());
    assert(command_seen("-C WIFI1B_APP_AP -j ACCEPT") ||
           command_seen("-I WIFI1B_APP_AP -j ACCEPT"));
    assert_no_command("-I INPUT -j ");
}

static void test_services_and_flush(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();

    test_dhcpd_start = 0;
    assert(!ap_open_services());
    assert(strstr(g_last_msg, "DHCP server failed") != NULL);

    test_dhcpd_start = 1;
    g_fail_substring = "tables";
    assert(!ap_open_services());
    assert(strstr(g_last_msg, "firewall setup failed") != NULL);

    test_dhcpd_start = -1;
    g_fail_substring = NULL;

    /* IPv4 cleanup is explicitly family-specific. */
    g_cmd_count = 0;
    w2_iface_flush();
    assert(command_seen("ip -4 addr flush dev wlan0"));
    assert_no_command("ip addr flush dev wlan0 ");
}

static void test_carplay_control_ownership(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();

    write_owner("wifi_daemon:999999\n");
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    g_cfg.carplay_control = 1;
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(apply_carplay_control(false));
    assert(g_cmd_count == 0);
    assert(g_cfg.mode == MODE_OFF && !g_cfg.wlan_on);
    assert(strstr(g_last_msg, "wifi_daemon") != NULL);

    g_cfg.carplay_control = 0;
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(apply_carplay_control(false));
    assert(g_cmd_count == 0);
    assert(strstr(g_last_msg, "wifi_daemon") != NULL);

    /* Stock handoff must not run any teardown/eviction command on a
     * peer-owned radio, and only the OEM manager commands when idle. */
    g_cmd_count = 0;
    carplay_stock_handoff(false);
    assert(g_cmd_count == 0);

    remove_owner();
    g_cmd_count = 0;
    carplay_stock_handoff(false);
    assert(command_seen("systemctl unmask supplicantmanager.path"));
    assert(command_seen("systemctl start supplicantmanager.path"));
    assert_no_command("pkill");
    assert_no_command("connmanctl tether");
    assert_no_command("ap_stop");
}

static void test_apply_mode_unowned(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    g_cmd_count = 0;
    assert(apply_mode_work(false));
    /* No reservation: no supplicant/AP/OEM-manager command may run. */
    assert(g_cmd_count == 0);
    assert_no_command("pkill");
    assert_no_command("connmanctl tether");
    assert_no_command("supplicantmanager");
    assert_no_command("wpa_supplicant");
    assert_no_command("ifconfig wlan0");
    assert_no_command("ip link set wlan0");

    g_cfg.carplay_control = 1;
    g_cmd_count = 0;
    assert(apply_mode_work(false));
    assert(g_cmd_count == 0);
    assert_no_command("supplicantmanager");
    assert_no_command("pkill");
    assert_no_command("connmanctl tether");

    /* Attempted OEM start failure is reported, not folded into success. */
    fixture_reset();
    test_owned_wpa = 0;
    remove_owner();
    g_cfg.carplay_control = 1;
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_fail_substring = "systemctl start supplicantmanager.path";
    g_cmd_count = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "handoff could not be confirmed") != NULL);

    fixture_reset();
    test_owned_wpa = 0;
    remove_owner();
    g_cfg.carplay_control = 1;
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_fail_substring = "systemctl unmask supplicantmanager";
    g_cmd_count = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "handoff could not be confirmed") != NULL);

    /* Exact-ledger route cleanup failure also fails closed. */
    fixture_reset();
    test_owned_wpa = 1;
    g_cfg.carplay_control = 0;
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_default_route_fixture = "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n";
    g_fail_substring = "route del default";
    g_cmd_count = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "route cleanup could not be confirmed") != NULL);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    g_fail_substring = NULL;

    /* Malformed ledger is never acted on and is not reported as success. */
    write_text(WIFI1B_CLIENT_ROUTE_LEDGER, "not a valid ledger\n");
    g_cmd_count = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "route cleanup could not be confirmed") != NULL);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    g_default_route_fixture = NULL;
    test_dhcpd_running = -1;
}


static void test_client_leave_cleanup(void)
{
    fixture_reset();
    radio_release();
    test_command = fixture_command;
    test_iface = 1;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.valid = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_default_route_fixture =
        "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n";
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_addr_del_effect = true;
    g_route_del_effect = true;
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    /* AP startup itself may fail in this fixture; the contract under test is
     * that the owned route and address are removed and verified first. */
    (void)apply_mode_work(false);
    assert(command_seen("route del default via 10.42.0.1 dev wlan0 metric 101 proto 66"));
    assert(command_seen("addr del 10.42.0.10/24 dev wlan0"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.ip == 0);
    assert(g_dhcp_lease.iface[0] == 0);

    /* A route cleanup failure blocks the AP start and retains the exact
     * owner for retry; it must not report the transition as clean. */
    fixture_reset();
    radio_release();
    test_dhcpd_running = 0;
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_default_route_fixture =
        "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n";
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_addr_del_effect = true;
    g_fail_substring = "route del default";
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "cleanup could not be confirmed") != NULL);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(!command_seen("iw reg set GB"));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    g_fail_substring = NULL;

    /* An address deletion failure blocks the AP start the same way. */
    fixture_reset();
    radio_release();
    test_dhcpd_running = 0;
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_default_route_fixture =
        "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n";
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_route_del_effect = true;
    g_fail_substring = "addr del";
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "cleanup could not be confirmed") != NULL);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(!command_seen("iw reg set GB"));
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    g_fail_substring = NULL;
    radio_release();
}

static void test_off_mode_pending_readiness(void)
{
    fixture_reset();
    radio_release();
    test_dhcpd_running = 0;
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_fail_substring = "addr del";
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "WLAN-off cleanup could not be confirmed") != NULL);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert_no_command("addr flush");
    g_fail_substring = NULL;

    /* Route-only pending is enough to block a clean Off/handoff claim. */
    fixture_reset();
    radio_release();
    test_dhcpd_running = 0;
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    write_test_route_ledger("10.42.0.1", "wlan0", 101, 66);
    g_default_route_fixture =
        "default via 10.42.0.1 dev wlan0 proto 66 metric 101\n";
    g_fail_substring = "route del default";
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "WLAN-off cleanup could not be confirmed") != NULL);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    g_fail_substring = NULL;

    /* A peer-owned idle radio cannot claim clean while our lease is pending. */
    fixture_reset();
    radio_release();
    test_dhcpd_running = 0;
    test_owned_wpa = 0;
    write_owner("wifi_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(!apply_mode_work(false));
    assert(strstr(g_last_msg, "owned lease cleanup pending") != NULL);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    radio_release();
}

static void test_disconnect_verified_status(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_fail_substring = "addr del";
    assert(!client_disconnect());
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);

    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_addr_del_effect = true;
    assert(client_disconnect());
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);

    /* stop_all reports the same verified cleanup status. */
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_fail_substring = "addr del";
    assert(!stop_all());
    assert(g_dhcp_lease.cleanup_pending);
    radio_release();
}

static bool mode_cleanup_work(work_t kind, bool pair)
{
    (void)pair;
    if (kind == WORK_DHCP)
        return w2_dhcp_cleanup_owned("wlan0", true);
    if (kind == WORK_MODE)
        return apply_mode_work(false);
    return true;
}

static void reap_until_idle_net(void)
{
    struct timespec ts = {0, 2000000};
    int64_t deadline = now_ms() + 5000;
    while (g_apply_pid > 0 && now_ms() < deadline) {
        apply_reap();
        if (g_apply_pid > 0) nanosleep(&ts, NULL);
    }
    assert(g_apply_pid == 0);
}

static void test_unowned_pending_worker_retry(void)
{
    fixture_reset();
    radio_release();
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 0;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    g_apply_pid = 0;
    g_work_pending = false;
    g_apply_cancelled = false;
    test_work = mode_cleanup_work;
    request_work(WORK_MODE, false);
    assert(g_apply_pid > 0);
    reap_until_idle_net();

    /* The worker result and the parent both retain the exact owner. */
    assert(!g_dhcp_lease.valid);
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(!strcmp(g_dhcp_lease.iface, "wlan0"));

    /* Once the radio is free, the bounded tick retries exact cleanup. */
    remove_owner();
    radio_release();
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_addr_del_effect = true;
    g_apply_pid = 0;
    g_work_pending = false;
    g_dhcp_work_step = W2_DHCP_RENEW;
    dhcp_lease_tick();
    assert(g_apply_pid > 0);
    assert(g_work_kind == WORK_DHCP);
    assert(g_dhcp_work_step == W2_DHCP_RELEASE);
    reap_until_idle_net();
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);
    test_work = NULL;
    radio_release();
}

static void test_shutdown_cleanup_status(void)
{
    fixture_reset();
    radio_release();
    test_command = fixture_command;
    test_iface = 0;
    test_owned_wpa = 0;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("pending", w2_ip4("10.42.0.10"),
                                 24, "wlan0"));
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_fail_substring = "addr del";

    /* Shutdown cleanup must report failure, keep the exact journal owner,
     * and never claim clean without readback. */
    assert(!shutdown_cleanup_work());
    assert(g_dhcp_lease.cleanup_pending);
    assert(!g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0);
    assert_no_command("addr flush");

    g_fail_substring = NULL;
    g_addr_del_effect = true;
    assert(shutdown_cleanup_work());
    assert(!g_dhcp_lease.cleanup_pending);
    assert(g_dhcp_lease.updated);
    assert(g_dhcp_lease.ip == 0);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
    radio_release();
}

static void test_shutdown_process_exit(void)
{
    fixture_reset();
    radio_release();
    test_command = fixture_command;
    test_iface = 0;
    test_owned_wpa = 0;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.10"), 24, "wlan0"));
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_fail_substring = "addr del";
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char *args[] = {(char *)"wifi1b_daemon_test",
                        (char *)"--test-sigterm-mainloop", NULL};
        int rc = daemon_main(2, args);
        _exit(rc);
    }
    struct timespec ts = {0, 150000000};
    nanosleep(&ts, NULL);
    assert(kill(pid, SIGTERM) == 0);
    int st = 0;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) != 0);

    fixture_reset();
    radio_release();
    test_command = fixture_command;
    test_iface = 0;
    test_owned_wpa = 0;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("current", w2_ip4("10.42.0.10"), 24, "wlan0"));
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_addr_del_effect = true;
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char *args[] = {(char *)"wifi1b_daemon_test",
                        (char *)"--test-sigterm-mainloop", NULL};
        int rc = daemon_main(2, args);
        _exit(rc);
    }
    nanosleep(&ts, NULL);
    assert(kill(pid, SIGTERM) == 0);
    st = 0;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    radio_release();
}

static void wait_path_short(const char *path)
{
    struct timespec ts = {0, 2000000};
    for (int i = 0; i < 5000; i++) {
        if (access(path, F_OK) == 0) return;
        nanosleep(&ts, NULL);
    }
    assert(0 && "timed out waiting for fixture marker");
}

static void radio_lineage_holder(int gen, const char *out,
                                 const char *release)
{
    w2_dhcp_lineage_t l;
    if (radio_begin("wifi1b_daemon", true) != 1) {
        write_text(out, "busy 0 none\n");
        _exit(2);
    }
    /* Marker is written while the real radio flock is held. */
    write_text(APP_DIR "/lineage-holder", "held\n");
    bool ok = w2_dhcp_lineage_ensure(gen, &l);
    char line[128];
    snprintf(line, sizeof(line), "%d %d %s\n", ok ? 1 : 0,
             l.high_water, l.token);
    write_text(out, line);
    wait_path_short(release);
    radio_release();
    _exit(ok ? 0 : 1);
}

static void test_shutdown_inflight_sigterm(void)
{
    fixture_reset();
    radio_release();
    test_command = fixture_command;
    test_iface = 0;
    test_owned_wpa = 0;
    test_dhcpd_running = 0;
    config_defaults();
    write_owner("wifi1b_daemon:999999\n");
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.carplay_control = 0;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.cleanup_pending = true;
    g_dhcp_lease.ip = w2_ip4("10.42.0.10");
    g_dhcp_lease.prefix = 24;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
    assert(w2_dhcp_journal_write("pending", w2_ip4("10.42.0.10"),
                                 24, "wlan0"));
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_addr_del_effect = true;
    unlink(APP_DIR "/sigterm-ready");
    pid_t daemon_pid = fork();
    assert(daemon_pid >= 0);
    if (daemon_pid == 0) {
        pid_t worker = fork();
        assert(worker >= 0);
        if (worker == 0) {
            setpgid(0, 0);
            sleep(60);
            _exit(0);
        }
        setpgid(worker, worker);
        g_apply_pid = worker;
        g_work_kind = WORK_DHCP;
        g_apply_dhcp_capable = true;
        g_txn_active = true;
        g_apply_start_ms = now_ms();
        g_apply_deadline_ms = g_apply_start_ms + 100;
        g_apply_cancelled = false;
        char *args[] = {(char *)"wifi1b_daemon_test",
                        (char *)"--test-sigterm-mainloop", NULL};
        int rc = daemon_main(2, args);
        _exit(rc);
    }
    wait_path_short(APP_DIR "/sigterm-ready");
    assert(kill(daemon_pid, SIGTERM) == 0);
    int st = 0;
    assert(waitpid(daemon_pid, &st, 0) == daemon_pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) != 0);
    unlink(APP_DIR "/sigterm-ready");
    fixture_reset();
    radio_release();
}

static void test_lineage_radio_lock_contract(void)
{
    fixture_reset();
    radio_release();
    unlink(WIFI1B_DHCP_LINEAGE);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(APP_DIR "/lineage-holder");
    unlink(APP_DIR "/lineage-release");
    unlink(APP_DIR "/lineage-a");
    unlink(APP_DIR "/lineage-b");
    unlink(APP_DIR "/lineage-c");
    test_lineage_require_lock = 1;

    /* Worker A holds the real radio flock; worker B must be refused. */
    pid_t a = fork();
    assert(a >= 0);
    if (a == 0)
        radio_lineage_holder(9, APP_DIR "/lineage-a",
                             APP_DIR "/lineage-release");
    wait_path_short(APP_DIR "/lineage-holder");
    pid_t b = fork();
    assert(b >= 0);
    if (b == 0) {
        w2_dhcp_lineage_t l;
        if (radio_begin("wifi1b_daemon", true) != 1) {
            write_text(APP_DIR "/lineage-b", "busy 0 none\n");
            _exit(2);
        }
        bool ok = w2_dhcp_lineage_ensure(7, &l);
        radio_release();
        char line[128];
        snprintf(line, sizeof(line), "%d %d %s\n", ok ? 1 : 0,
                 l.high_water, l.token);
        write_text(APP_DIR "/lineage-b", line);
        _exit(ok ? 0 : 1);
    }
    int stb = 0;
    assert(waitpid(b, &stb, 0) == b);
    assert(WIFEXITED(stb) && WEXITSTATUS(stb) != 0);
    write_text(APP_DIR "/lineage-release", "go\n");
    int sta = 0;
    assert(waitpid(a, &sta, 0) == a);
    assert(WIFEXITED(sta) && WEXITSTATUS(sta) == 0);

    w2_dhcp_lineage_t final;
    assert(w2_dhcp_lineage_load(&final) == 1);
    assert(final.high_water == 9);
    char line[128] = "";
    assert(read_file(APP_DIR "/lineage-a", line, sizeof(line)) > 0);
    char token[40] = "";
    int ok_a = 0, hw_a = 0;
    assert(sscanf(line, "%d %d %39s", &ok_a, &hw_a, token) == 3);
    assert(ok_a == 1 && hw_a == 9 && !strcmp(token, final.token));

    /* A later gen=7 worker with the real lock cannot regress high-water. */
    unlink(APP_DIR "/lineage-holder");
    unlink(APP_DIR "/lineage-release");
    pid_t c = fork();
    assert(c >= 0);
    if (c == 0)
        radio_lineage_holder(7, APP_DIR "/lineage-c",
                             APP_DIR "/lineage-release");
    wait_path_short(APP_DIR "/lineage-holder");
    write_text(APP_DIR "/lineage-release", "go\n");
    int stc = 0;
    assert(waitpid(c, &stc, 0) == c);
    assert(WIFEXITED(stc) && WEXITSTATUS(stc) == 0);
    assert(w2_dhcp_lineage_load(&final) == 1);
    assert(final.high_water == 9);
    assert(!strcmp(final.token, token));
    test_lineage_require_lock = 0;
    unlink(APP_DIR "/lineage-holder");
    unlink(APP_DIR "/lineage-release");
    unlink(APP_DIR "/lineage-a");
    unlink(APP_DIR "/lineage-b");
    unlink(APP_DIR "/lineage-c");
    radio_release();
}

static void test_ap_probe_without_interface(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 0;
    config_defaults();
    ap_probe_t p;
    ap_probe(&p);
    assert(p.role == 0);
    assert(p.state == AP_ABSENT);
    assert(p.ip[0] == 0 && p.linklocal[0] == 0);
}

static int lease_slot_for(const uint8_t mac[6])
{
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++)
        if (g_w2_leases[i].used && !memcmp(g_w2_leases[i].mac, mac, 6))
            return (int)i;
    return -1;
}

static void test_dhcp_release_gates(void)
{
    uint8_t opts[312], mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    uint8_t *cid;
    size_t discover_len, request_len;

    assert(w2_mask_to_prefix(0xffffff00U) == 24);
    assert(w2_mask_to_prefix(0xffffffffU) == 32);
    assert(w2_mask_to_prefix(0x80000000U) == 1);
    assert(w2_mask_to_prefix(0xffff00ffU) == -1);

    memset(opts, 0, sizeof(opts));
    discover_len = w2_dhcp_client_opts(opts, sizeof(opts), 1, mac, 0, 0, false);
    cid = w2_dhcp_opt(opts, discover_len, 61);
    assert(cid != NULL && cid[0] == 1 && !memcmp(cid + 1, mac, 6));
    assert(w2_dhcp_opt(opts, discover_len, 53) != NULL);
    assert(w2_dhcp_opt(opts, discover_len, 55) != NULL);

    memset(opts, 0, sizeof(opts));
    request_len = w2_dhcp_client_opts(opts, sizeof(opts), 3, mac,
                                      w2_ip4("10.42.0.10"),
                                      w2_ip4(WIFI1B_AP_IP), true);
    cid = w2_dhcp_opt(opts, request_len, 61);
    assert(cid != NULL && cid[0] == 1 && !memcmp(cid + 1, mac, 6));
    assert(w2_dhcp_opt(opts, request_len, 50) != NULL);
    assert(w2_dhcp_opt(opts, request_len, 54) != NULL);
    assert(w2_dhcp_opt(opts, request_len, 55) != NULL);

    fixture_reset();
    test_command = fixture_command;
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_route4_fixture =
        "10.42.0.0/24 dev wlan0 proto kernel scope link src 10.42.0.10\n";
    assert(w2_addr_present("wlan0", "10.42.0.10", 24));
    assert(!w2_addr_present("wlan0", "10.42.0.11", 24));
    assert(w2_connected_route_present("wlan0", "10.42.0.10",
                                      0xffffff00U, 24));
    assert(!w2_connected_route_present("wlan0", "10.42.0.10",
                                       0xffffff00U, 25));

    memset(g_w2_leases, 0, sizeof(g_w2_leases));
    time_t wall = time(NULL);
    char leases[1024], path[512];
    snprintf(path, sizeof(path), "%s", WIFI1B_DHCP_LEASE_FILE);
    snprintf(leases, sizeof(leases),
             "# mac ip expires_unix\n"
             "02:00:11:22:33:44 10.42.0.10 %lld\n"
             "02:00:11:22:33:45 10.42.0.11 %lld\n"
             "02:00:11:22:33:46 10.42.0.250 %lld\n"
             "garbage line\n",
             (long long)wall + 3600, (long long)wall - 10,
             (long long)wall + 3600);
    write_text(path, leases);
    g_w2_leases[0].used = false;
    w2_lease_load();
    uint8_t mac1[6], mac2[6], mac3[6];
    assert(parse_mac6("02:00:11:22:33:44", mac1));
    assert(parse_mac6("02:00:11:22:33:45", mac2));
    assert(parse_mac6("02:00:11:22:33:46", mac3));
    assert(w2_lease_for_mac(mac1, now_ms()) == w2_ip4("10.42.0.10"));
    assert(w2_lease_for_mac(mac2, now_ms()) == 0);
    assert(w2_lease_for_mac(mac3, now_ms()) == 0);
    int slot = lease_slot_for(mac1);
    assert(slot >= 0);
    int64_t before = g_w2_leases[slot].expires_ms;
    assert(before > now_ms() + 3500000);
    g_w2_leases[slot].expires_ms = now_ms() + 1000;
    assert(w2_lease_for_mac(mac1, now_ms()) == w2_ip4("10.42.0.10"));
    assert(g_w2_leases[slot].expires_ms > now_ms() + 3500000);
    w2_lease_write();
    char saved[2048];
    assert(read_file(path, saved, sizeof(saved)) > 0);
    assert(strstr(saved, "02:00:11:22:33:44 10.42.0.10"));
    assert(strstr(saved, "02:00:11:22:33:45") == NULL);
    assert(strstr(saved, "10.42.0.250") == NULL);
}

static void test_identity_gate(void)
{
    char saved_id[sizeof(g_cfg.cp_id)];
    char saved_bt[sizeof(g_cfg.cp_btaddr)];
    char status[8192];

    str_copy(saved_id, sizeof(saved_id), g_cfg.cp_id);
    str_copy(saved_bt, sizeof(saved_bt), g_cfg.cp_btaddr);
    str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), "AA:BB:CC:DD:EE:FF");

    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "bad\"id\nnetwork={");
    assert(!carplay_ie_on());
    assert(!g_carplay_ie);
    build_status(status, sizeof(status));
    assert(strstr(status, "\"carplay_id\":\"\"") != NULL);

    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "AA:BB:CC:DD:EE:FF");
    assert(!carplay_ie_on());
    assert(!g_carplay_ie);

    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "AA:BB:CC:DD:EE:01");
    assert(carplay_ie_on());
    assert(g_carplay_ie);

    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), saved_id);
    str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), saved_bt);
    g_carplay_ie = false;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--flush")) {
        w2_iface_flush();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--fw-open")) {
        config_defaults();
        return carplay_fw_open() ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--fw-open-ap")) {
        config_defaults();
        return ap_fw_open() ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--query-status")) {
        fixture_reset();
        test_command = fixture_command;
        set_ap_fixtures();
        prepare_ipv6_fixture(
            "fe800000000000000000000000000003 03 40 20 80    wlan0\n", 0);
        config_defaults();
        char *args[] = {(char *)"wifi1b_daemon_test", (char *)"--status", NULL};
        return daemon_main(2, args);
    }
    if (argc > 1 && !strcmp(argv[1], "--query-port")) {
        fixture_reset();
        test_command = fixture_command;
        config_defaults();
        char *args[] = {(char *)"wifi1b_daemon_test", (char *)"--choose-port",
                        (char *)"2224", NULL};
        return daemon_main(3, args);
    }

    mkdir(APP_DIR, 0700);
    test_linklocal_parser();
    test_command_parsers();
    test_probe_readiness_and_status();
    test_ipv6_failures();
    test_firewall_rules();
    test_services_and_flush();
    test_identity_gate();
    test_carplay_control_ownership();
    test_apply_mode_unowned();
    test_client_leave_cleanup();
    test_off_mode_pending_readiness();
    test_disconnect_verified_status();
    test_unowned_pending_worker_retry();
    test_shutdown_cleanup_status();
    test_shutdown_process_exit();
    test_shutdown_inflight_sigterm();
    test_lineage_radio_lock_contract();
    test_dhcp_release_gates();
    test_ap_probe_without_interface();
    puts("PASS: WiFi1b C2 AP IPv4/IPv6 readiness, readback, firewall and ownership helpers");
    return 0;
}
