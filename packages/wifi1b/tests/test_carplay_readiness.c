/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Offline readiness/firewall/status checks for the Wifi1b daemon. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }

static void fw_line(char *out, size_t n, const char *chain, const char *proto,
                    int port)
{
    snprintf(out, n, "-A %s -p %s -m %s --dport %d -j ACCEPT\n",
             chain, proto, proto, port);
}

static void append_fw(char *text, size_t n, bool v6)
{
    const char *marker = v6 ? "__WIFI1B_V6__\n" : "__WIFI1B_V4__\n";
    strcat(text, marker);
    strcat(text, v6 ? "-A INPUT -i wlan0 -j WIFI1B_APP_CP\n"
                    : "-A INPUT -i wlan0 -j WIFI1B_APP_CP\n");
    strcat(text, v6 ? "-A OUTPUT -o wlan0 -j WIFI1B_APP_CP\n"
                    : "-A OUTPUT -o wlan0 -j WIFI1B_APP_CP\n");
    const int udp[] = {v6 ? 547 : 67, 5353, 5020, 6000, 6003, 6011};
    const int tcp[] = {5000, 5001, 6030, 7000};
    char line[160];
    for (size_t i = 0; i < sizeof(udp) / sizeof(udp[0]); i++) {
        fw_line(line, sizeof(line), "WIFI1B_APP_CP", "udp", udp[i]);
        strcat(text, line);
    }
    for (size_t i = 0; i < sizeof(tcp) / sizeof(tcp[0]); i++) {
        fw_line(line, sizeof(line), "WIFI1B_APP_CP", "tcp", tcp[i]);
        strcat(text, line);
    }
    if (v6) {
        strcat(text, "-A WIFI1B_APP_CP -p ipv6-icmp -j ACCEPT\n");
        strcat(text, "-A WIFI1B_APP_CP -d ff00::/8 -j ACCEPT\n");
    }
    (void)n;
}

static void test_config_toggle(void)
{
    config_defaults();
    assert(!g_cfg.cp_vendor_ie);
    const char *text = "mode=off\nwlan_on=0\ncarplay_vendor_ie=1\n";
    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    config_load();
    assert(g_cfg.cp_vendor_ie);
    assert(config_save());
    char saved[2048];
    assert(read_file(CONFIG_PATH, saved, sizeof(saved)) > 0);
    assert(strstr(saved, "carplay_vendor_ie=1"));
}

static void test_gate(void)
{
    bool components = true, session = true;
    assert(!strcmp(carplay_gate_name(false, true, false, true, true, true,
                                     true, true, true, true, 1,
                                     &components, &session), "inactive"));
    assert(!components && !session);
    assert(!strcmp(carplay_gate_name(true, true, false, false, true, true,
                                     true, true, true, true, 1,
                                     &components, &session), "ap-snapshot"));
    assert(!components && !session);
    assert(!strcmp(carplay_gate_name(true, true, false, true, false, true,
                                     true, true, true, true, 1,
                                     &components, &session), "ap-readback"));
    assert(!components && !session);
    /* Missing owned chain: not ready even with a fresh AP snapshot. */
    assert(!strcmp(carplay_gate_name(true, true, false, true, true, false,
                                     true, true, true, true, 1,
                                     &components, &session), "firewall"));
    assert(!components && !session);
    assert(!strcmp(carplay_gate_name(true, true, false, true, true, true,
                                     true, true, true, false, 1,
                                     &components, &session), "spp"));
    assert(components && !session);
    assert(!strcmp(carplay_gate_name(true, true, false, true, true, true,
                                     true, true, true, true, 0,
                                     &components, &session), "association"));
    assert(components && !session);
    assert(!strcmp(carplay_gate_name(true, true, false, true, true, true,
                                     true, true, true, true, 1,
                                     &components, &session), "projection"));
    assert(components && session);
}

static void test_firewall_text(void)
{
    char text[8192] = "";
    append_fw(text, sizeof(text), false);
    append_fw(text, sizeof(text), true);
    assert(carplay_fw_text_ok(text));
    char broken[8192];
    str_copy(broken, sizeof(broken), text);
    char *p = strstr(broken, "--dport 6030");
    assert(p != NULL);
    /* Removing the IPv6 6030 rule must fail the contract. */
    char *line = p;
    while (line > broken && line[-1] != '\n') line--;
    char *end = strchr(p, '\n');
    assert(end != NULL);
    memmove(line, end + 1, strlen(end + 1) + 1);
    assert(!carplay_fw_text_ok(broken));
    str_copy(broken, sizeof(broken), text);
    p = strstr(broken, "-A OUTPUT -o wlan0 -j WIFI1B_APP_CP");
    assert(p != NULL);
    p[0] = 'X';
    assert(!carplay_fw_text_ok(broken));
}

static void write_text(const char *path, const char *text, time_t age)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    struct timespec ts[2];
    ts[0].tv_sec = time(NULL) - age;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    assert(utimensat(AT_FDCWD, path, ts, 0) == 0);
}

static void test_broker_status(void)
{
    observation_t o;
    const char *recent =
        "{\"ok\":true,\"state\":\"running\",\"error\":\"\",\"epoch\":2,"
        "\"peer\":\"AA:BB:CC:DD:EE:FF\",\"ap_on_air\":true,"
        "\"usb_blocked\":false,\"receiver_observed\":true,"
        "\"rx_packets\":3,\"rx_dropped\":0,\"tx_chunks\":4}\n";
    memset(&o, 0, sizeof(o));
    write_text(CARPLAY_BROKER_STATUS, recent, 0);
    observe_broker_status(&o);
    assert(o.broker_observed);
    assert(!strcmp(o.broker_state, "running"));
    write_text(CARPLAY_BROKER_STATUS, recent, 60);
    memset(&o, 0, sizeof(o));
    observe_broker_status(&o);
    assert(!o.broker_observed);
    const char *no_rx =
        "{\"state\":\"running\",\"peer\":\"AA:BB:CC:DD:EE:FF\","
        "\"ap_on_air\":true,\"receiver_observed\":true,\"rx_packets\":0}\n";
    write_text(CARPLAY_BROKER_STATUS, no_rx, 0);
    memset(&o, 0, sizeof(o));
    observe_broker_status(&o);
    assert(!o.broker_observed);
    const char *not_running =
        "{\"state\":\"receiver_incomplete\","
        "\"peer\":\"AA:BB:CC:DD:EE:FF\",\"ap_on_air\":true,"
        "\"receiver_observed\":true,\"rx_packets\":3}\n";
    write_text(CARPLAY_BROKER_STATUS, not_running, 0);
    memset(&o, 0, sizeof(o));
    observe_broker_status(&o);
    assert(!o.broker_observed);
    assert(!strcmp(o.broker_state, "receiver_incomplete"));
}

static void test_apply_state(void)
{
    bool components = false, session = false;
    g_carplay_last_apply_ok = true;
    g_carplay_last_apply_fw_ok = true;
    g_carplay_last_apply_at = 1234;
    carplay_state_save();
    g_carplay_last_apply_ok = false;
    g_carplay_last_apply_fw_ok = false;
    g_carplay_last_apply_at = 0;
    carplay_state_load();
    assert(g_carplay_last_apply_ok);
    assert(g_carplay_last_apply_fw_ok);
    assert(g_carplay_last_apply_at == 1234);

    /* Restart after a vendor firewall reload: the persisted apply result is
     * still reported, but a fresh readback with a missing owned chain keeps
     * readiness false and names the firewall gate. */
    assert(!strcmp(carplay_gate_name(true, true, false, true, true, false,
                                     true, true, true, true, 1,
                                     &components, &session), "firewall"));
    assert(!components && !session);
    assert(g_carplay_last_apply_ok && g_carplay_last_apply_fw_ok);
    assert(!strcmp(carplay_gate_name(true, true, false, true, true, true,
                                     true, true, true, true, 1,
                                     &components, &session), "projection"));
    assert(components && session);
}

int main(void)
{
    test_config_toggle();
    test_gate();
    test_firewall_text();
    test_broker_status();
    test_apply_state();
    puts("PASS: CarPlay readiness gates, firewall readback, broker status and apply persistence");
    return 0;
}
