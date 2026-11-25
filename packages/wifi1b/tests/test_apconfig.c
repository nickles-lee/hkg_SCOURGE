/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Offline checks for Wifi1b AP/station config generation and CarPlay ID. */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }

int main(void)
{
    mkdir(APP_DIR, 0700);
    config_defaults();
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), "fixture-ap-key");
    str_copy(g_cfg.cp_name, sizeof(g_cfg.cp_name), "Ioniq 5");
    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "aa:bb:cc:dd:ee:ff");
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;
    assert(write_ap_conf());
    char conf[8192];
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "frequency=5180"));
    assert(strstr(conf, "mode=2"));
    assert(strstr(conf, "ap_scan=2"));
    assert(strstr(conf, "app_info=1"));
    assert(strstr(conf, "carplay_id=aa:bb:cc:dd:ee:ff"));
    assert(strstr(conf, "ap_vendor_elements=") == NULL);
    g_cfg.cp_vendor_ie = true;
    assert(write_ap_conf());
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "ap_vendor_elements=dd0800a0400000020021"));
    g_cfg.cp_band = 1;
    g_cfg.cp_channel = 6;
    assert(write_ap_conf());
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "ap_vendor_elements=dd0800a0400000020022"));
    g_cfg.cp_vendor_ie = false;
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;
    assert(strstr(conf, "app_info=1") < strstr(conf, "network={"));
    assert(strstr(conf, "carplay_id=") < strstr(conf, "network={"));
    assert(strstr(conf, "    app_info=") == NULL);
    assert(strstr(conf, "    carplay_id=") == NULL);
    assert(strstr(conf, "carplay_name=\"Ioniq 5\""));
    assert(strstr(conf, "key_mgmt=WPA-PSK"));
    assert(strstr(conf, "ssid=\"IONIQ5-CarPlay-B\""));
    assert(!strstr(conf, "\npsk=fixture-ap-key"));

    g_cfg.cp_band = 1;
    g_cfg.cp_channel = 6;
    assert(write_ap_conf());
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "frequency=2437"));

    /* The Bluetooth MAC never replaces an explicit/distinct AP identity. */
    str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), "aa:bb:cc:dd:ee:ff");
    g_cfg.cp_id[0] = '\0';
    prepare_carplay_identity();
    assert(!g_cfg.cp_id[0] || strcmp(g_cfg.cp_id, g_cfg.cp_btaddr) != 0);
    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "SCOURGE-AP-ID");
    prepare_carplay_identity();
    assert(strcmp(g_cfg.cp_id, "SCOURGE-AP-ID") == 0);
    assert(strcmp(g_cfg.cp_id, g_cfg.cp_btaddr) != 0);
    assert(write_ap_conf());
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "carplay_id=SCOURGE-AP-ID"));

    /* File/config-derived ids are validated; a quote/newline cannot inject a
     * second supplicant directive.  The field is omitted, not rewritten. */
    assert(!carplay_id_allowed("bad\"id\nnetwork={\n"));
    assert(!carplay_id_allowed("bad id"));
    assert(!carplay_id_allowed(""));
    assert(carplay_id_allowed("SCOURGE-AP_ID.1:2"));
    str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "bad\"id\nnetwork={\n");
    prepare_carplay_identity();
    assert(write_ap_conf());
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "carplay_id=") == NULL);
    assert(strstr(conf, "bad") == NULL);
    assert(strstr(conf, "\nnetwork={\nnetwork={\n") == NULL);

    assert(write_station_conf("iPhone", "fixture-client-key", false));
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "key_mgmt=WPA-PSK"));
    assert(strstr(conf, "ssid=\"iPhone\""));
    assert(strstr(conf, "scan_ssid=1"));

    assert(write_station_conf("iPhone", "fixture-client-key", true));
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "key_mgmt=SAE"));
    assert(strstr(conf, "ieee80211w=2"));
    str_copy(g_cfg.extra_ssid[0], sizeof(g_cfg.extra_ssid[0]), "HomeFixture");
    str_copy(g_cfg.extra_psk[0], sizeof(g_cfg.extra_psk[0]), "backup-fixture-key");
    assert(write_station_conf("iPhone", "fixture-client-key", false));
    assert(read_file(WPA_CONF, conf, sizeof(conf)) > 0);
    assert(strstr(conf, "ssid=\"iPhone\"") < strstr(conf, "ssid=\"HomeFixture\""));
    assert(strstr(conf, "priority=100") && strstr(conf, "priority=99"));
    assert(!write_station_conf("iPhone", "short", false));

    unsigned char mac[6];
    assert(parse_mac6("aa:bb:cc:dd:ee:ff", mac));
    assert(mac[0] == 0xaa && mac[5] == 0xff);
    assert(parse_mac6("AA-BB-CC-DD-EE-FF", mac));
    assert(!parse_mac6("aa:bb:cc", mac));
    assert(!parse_mac6("aa:bb:cc:dd:ee:ff:00", mac));

    uint8_t opts[] = {53, 1, 3, 50, 4, 10, 42, 0, 200, 255};
    assert(w2_dhcp_opt(opts, sizeof(opts), 53) && *w2_dhcp_opt(opts, sizeof(opts), 53) == 3);
    uint8_t *ip = w2_dhcp_opt(opts, sizeof(opts), 50);
    assert(ip && ip[0] == 10 && ip[3] == 200);
    assert(!w2_dhcp_opt(opts, sizeof(opts), 51));
    assert(w2_ip4("10.42.0.1") == 0x0a2a0001U);
    char ipstr[32];
    w2_ip4_str(0x0a2a0001U, ipstr, sizeof(ipstr));
    assert(!strcmp(ipstr, "10.42.0.1"));

    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;
    assert(write_ap_conf());
    puts("PASS: Wifi1b AP uses GB/5GHz, station config supports WPA2/SAE, IDs and DHCP helpers");
    return 0;
}
