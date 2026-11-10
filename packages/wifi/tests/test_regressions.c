/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }
static int commands;
static bool powered;
static bool fail_ap_power;
static int ap_requests;
static bool ap_active, stuck_off, unknown_role, no_actual_ssid, deny_tether;
static int success_request, tether_field = -1, active_probes, delay_ip, lose_after, off_requests, disconnects;
static int power_enables;
static bool transient_ip;
static bool ap_after_off, station_intrusion;
static bool saw_ap_scan, saw_iwconfig;
static const char *services_reply;
static const char *iw_reply, *wext_reply, *ip_reply;
static const char *service_reply;
static const char *station_status;

static int mock_command(char *out, size_t n, const char *cmd)
{
    commands++;
    if (strstr(cmd, "connmanctl enable wifi")) power_enables++;
    if (strstr(cmd, "get ap_scan")) saw_ap_scan = true;
    if (strstr(cmd, "iwconfig wlan0")) saw_iwconfig = true;
    if (strstr(cmd, "connmanctl tether wifi on ")) {
        ap_requests++;
        if (fail_ap_power) powered = false;
        if (success_request && ap_requests == success_request) ap_active = true;
        if (station_intrusion) station_status = "mode=station\nwpa_state=COMPLETED\nssid=Unexpected\n";
    }
    if (strstr(cmd, "connmanctl tether wifi off")) {
        off_requests++;
        if (!stuck_off) ap_active = false;
        if (ap_after_off) ap_active = true;
    }
    if (strstr(cmd, "connmanctl disconnect ")) {
        assert(strstr(cmd, "disconnect wifi_") && !strstr(cmd, "cellular_"));
        disconnects++;
        station_status = NULL;
    }
    if (strstr(cmd, "iw dev wlan0 info") && ap_active) {
        active_probes++;
        if (lose_after && active_probes >= lose_after) ap_active = false;
    }
    const char *reply = "";
    if (strstr(cmd, "wpa_cli") && strstr(cmd, "status")) reply = station_status ? station_status :
        ap_active && !unknown_role ? no_actual_ssid ? "mode=AP\nwpa_state=COMPLETED\n" :
        "mode=AP\nwpa_state=COMPLETED\nssid=IONIQ5-CarPlay\n" : "wpa_state=DISCONNECTED\n";
    else if (strstr(cmd, "ping")) reply = "PONG\n";
    else if (strstr(cmd, "services wifi_c_49646c65_managed_psk")) reply = "State = idle\n";
    else if (strstr(cmd, "services wifi_")) reply = service_reply;
    else if (strstr(cmd, "connmanctl services ")) reply = services_reply;
    else if (strstr(cmd, "iwconfig wlan0")) reply = wext_reply;
    else if (strstr(cmd, "iw dev wlan0 info")) reply = iw_reply ? iw_reply : unknown_role ? "" : ap_active ?
        no_actual_ssid ? "type AP\n" : "type AP\nssid IONIQ5-CarPlay\n" : "type managed\n";
    else if (strstr(cmd, "ifconfig wlan0")) reply = ip_reply ? ip_reply : station_intrusion && station_status ? "inet addr:192.0.2.10\n" : ap_active && active_probes > delay_ip ?
        "inet addr:10.42.0.1\n" : transient_ip ? "inet addr:192.0.2.10\n" : "";
    else if (strstr(cmd, "tether wifi on ") && deny_tether) reply = "Error enabling wifi tethering: Permission denied\n";
    else if (strstr(cmd, "technologies")) reply = tether_field == 1 || (ap_active && tether_field < 0) ?
        "/net/connman/technology/wifi\n  Powered = True\n  Tethering = True\n  TetheringIdentifier = IONIQ5-CarPlay\n" : powered ?
        "/net/connman/technology/wifi\n  Powered = True\n  Tethering = False\n  TetheringIdentifier = IONIQ5-CarPlay\n/net/connman/technology/cellular\n  Tethering = True\n" :
        "/net/connman/technology/wifi\n  Powered = False\n  Tethering = False\n";
    if (out && n) str_copy(out, n, reply ? reply : "");
    /* A second association owner must never be introduced. */
    assert(!strstr(cmd, "add_network") && !strstr(cmd, "remove_network") && !strstr(cmd, "set_network"));
    assert(!strstr(cmd, "-i wlan0 disconnect"));
    if (strstr(cmd, " -D INPUT ")) assert(strstr(cmd, " -j WIFI_APP_"));
    assert(!strstr(cmd, "autoconnect") && !strstr(cmd, "AutoConnect"));
    return 0;
}

static void wait_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static bool slow_work(work_t kind, bool pair)
{
    (void)kind; (void)pair;
    wait_ms(250);
    if (g_stop) return false;
    write_file_atomic(APP_DIR "/effective-ssh", g_cfg.ssh_enabled ? "1" : "0", 0600);
    g_carplay_ie = g_cfg.wlan_on && g_cfg.mode == MODE_CARPLAY;
    set_msg("test work finished");
    return true;
}

static void pump(void)
{
    int64_t deadline = now_ms() + 4000;
    while ((g_apply_pid || g_work_pending) && now_ms() < deadline) {
        apply_reap(); wait_ms(10);
    }
    assert(!g_apply_pid && !g_work_pending);
}

static int fake_connmanctl(void)
{
    char line[512];
    assert(fgets(line, sizeof(line), stdin) && !strcmp(line, "agent on\n"));
    fputs("Agent registered\n", stdout); fflush(stdout);
    assert(fgets(line, sizeof(line), stdin) && !strncmp(line, "connect wifi_", 13));
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    assert(poll(&p, 1, 150) == 0); /* no password before RequestInput */
    fputs("Pass", stdout); fflush(stdout);
    assert(poll(&p, 1, 150) == 0); /* a partial prompt is not enough */
    fputs("phrase? ", stdout); fflush(stdout);
    assert(fgets(line, sizeof(line), stdin) && !strcmp(line, "fixture-password\n"));
    fputs("Connected wifi_fixture\n", stdout); fflush(stdout);
    return 0;
}

static void reset_ap_fixture(void)
{
    ap_active = stuck_off = unknown_role = no_actual_ssid = deny_tether = false;
    transient_ip = ap_after_off = station_intrusion = false;
    success_request = ap_requests = active_probes = delay_ip = lose_after = off_requests = disconnects = 0;
    power_enables = 0;
    tether_field = -1;
    services_reply = station_status = NULL;
    iw_reply = wext_reply = ip_reply = NULL;
    powered = true;
    unlink(AP_STATION_IP);
    g_cfg.mode = MODE_CARPLAY; g_cfg.wlan_on = true;
    g_cfg.cp_band = 2; g_cfg.cp_channel = 36;
}

static void test_ap_states(void)
{
    ap_probe_t p;
    observation_t o;
    char scan[4096];
    reset_ap_fixture();
    success_request = 1; tether_field = 0;
    assert(ap_start() && ap_requests == 1 && off_requests == 0);
    collect_observation(&o);
    assert(o.ap_on && o.ap.role == 1 && o.ap.tether == 0 && !strcmp(o.ip, "10.42.0.1"));
    assert(ap_start() && ap_requests == 1 && off_requests == 0); /* reuse */
    assert(!ap_disable(false) && off_requests == 0); /* late AP before fallback */
    g_cfg.mode = MODE_CLIENT;
    scan_networks(scan, sizeof(scan));
    assert(strstr(scan, "AP absence not confirmed") && off_requests == 0);

    reset_ap_fixture(); success_request = 1; tether_field = 0; delay_ip = 28;
    assert(ap_start() && active_probes >= 30 && ap_requests == 1 && off_requests == 0);

    reset_ap_fixture(); success_request = 1; lose_after = 2;
    assert(!ap_start() && ap_requests == 1 && off_requests == 0);
    assert(strstr(g_last_msg, "enabled_then_lost"));

    reset_ap_fixture(); success_request = 1; no_actual_ssid = true;
    assert(!ap_start() && ap_requests == 1 && off_requests == 0);
    ap_probe(&p);
    assert(p.state == AP_INTERFACE_UP && !p.ssid[0]); /* configured SSID is not actual */

    reset_ap_fixture(); unknown_role = true;
    assert(!ap_start() && ap_requests == 0 && off_requests == 0 && power_enables == 0);
    ap_probe(&p); assert(p.state == AP_UNKNOWN);
    reset_ap_fixture();
    services_reply = "Error accessing service list: Permission denied\n";
    assert(!ap_start() && ap_requests == 0 && off_requests == 0);

    reset_ap_fixture(); iw_reply = "";
    wext_reply = "wlan0 ESSID:\"IONIQ5-CarPlay\" Mode:Master\n";
    ip_reply = "inet addr:10.42.0.1\n";
    ap_probe(&p); assert(p.state == AP_READY && p.tether == 0); /* WEXT fallback */
    wext_reply = NULL;
    station_status = "mode=AP\nwpa_state=COMPLETED\nssid=IONIQ5-CarPlay\n";
    ap_probe(&p); assert(p.state == AP_READY); /* completed supplicant AP fallback */
    iw_reply = "type AP\nssid IONIQ5-CarPlay\\x00bad\n";
    station_status = NULL;
    ap_probe(&p); assert(p.state == AP_INTERFACE_UP && !p.ssid[0]); /* malformed SSID is not partial success */
    iw_reply = "type AP\nssid IONIQ5-CarPlay\n";
    ip_reply = "inet addr:169.254.1.2\n";
    ap_probe(&p); assert(p.state == AP_INTERFACE_UP);

    reset_ap_fixture(); ap_after_off = true;
    assert(!ap_start() && ap_requests == 1 && off_requests == 1);
    assert(strstr(g_last_msg, "teardown did not finish"));

    reset_ap_fixture(); deny_tether = true;
    assert(!ap_start() && ap_requests == 1 && off_requests == 0);
    assert(strstr(g_last_msg, "permission denied")); /* rc=0 is not success */
    reset_ap_fixture(); success_request = 1; deny_tether = true; tether_field = 0;
    assert(ap_start() && ap_requests == 1 && off_requests == 0); /* actual ready wins */

    reset_ap_fixture(); success_request = 1; station_intrusion = true;
    services_reply = "*AR Other wifi_b_4f74686572_managed_psk\n*AR DATA cellular_123_context1\n";
    service_reply = "State = online\n";
    assert(ap_start() && disconnects == 2 && ap_requests == 1 && off_requests == 0);

    reset_ap_fixture(); transient_ip = true;
    station_status = "mode=station\nwpa_state=COMPLETED\nssid=Other\n";
    services_reply = "*AR Other wifi_b_4f74686572_managed_psk\n    Idle wifi_c_49646c65_managed_psk\n*AR DATA cellular_123_context1\n";
    service_reply = "State = ready\n";
    success_request = 1;
    assert(ap_start() && disconnects == 1); /* other active SSID, but not idle/cellular */
    char old_ip[64]; assert(read_file(AP_STATION_IP, old_ip, sizeof(old_ip)) > 0 && !strcmp(old_ip, "192.0.2.10"));
    assert(write_file_atomic(AP_STATION_IP, "10.42.0.1", 0600) == 0);
    ap_probe(&p); assert(p.state == AP_INTERFACE_UP); /* reject leftover station address */
    assert(!ssh_start() && !g_ssh_active_port && strstr(g_last_msg, "role/address not ready"));

    reset_ap_fixture(); transient_ip = true;
    station_status = "mode=station\nwpa_state=COMPLETED\nssid=Other\n";
    collect_observation(&o);
    assert(!o.ap_on && !o.ip[0]);
    assert(!ssh_start() && strstr(g_last_msg, "role/address not ready"));

    reset_ap_fixture(); ap_active = stuck_off = true; tether_field = 0;
    assert(!ap_stop());
    g_ap_restart_requested = true;
    assert(!apply_mode_work(false) && ap_requests == 0 && strstr(g_last_msg, "explicit restart blocked"));
    g_ap_restart_requested = false;
    g_cfg.mode = MODE_OFF; g_cfg.wlan_on = false;
    test_owned_wpa = 0; powered = false; /* AP is the only remaining Off blocker */
    assert(!apply_mode_work(false) && strstr(g_last_msg, "could not be confirmed"));
    test_owned_wpa = 1;
    reset_ap_fixture();

    /* Parent watchdog uses fresh role observations, never a transient IP
     * or a missing/stale probe as justification to start SSH/reset WLAN. */
    g_cfg.ssh_enabled = true;
    g_boot_dumped = true; g_network_failed = false;
    g_last_mode_ms = now_ms() - 120000;
    g_carplay_ie = true;
    g_observed = (observation_t){.gen = g_apply_gen, .ap_on = true, .carplay_svc = true,
        .mdns_svc = true, .ssh_ours = true, .ap = {.state = AP_READY, .role = 1, .tether = 0}};
    str_copy(g_observed.ip, sizeof(g_observed.ip), "10.42.0.1");
    g_observed_ms = now_ms(); g_last_watchdog_ms = 0;
    watchdog(); assert(!g_apply_pid && !g_work_pending);
    g_observed.ap_on = false; g_observed.ssh_ours = false;
    g_observed.ap = (ap_probe_t){.state = AP_ABSENT, .role = 0, .tether = 0};
    str_copy(g_observed.ip, sizeof(g_observed.ip), "192.0.2.10");
    g_ap_unready_ms = 0; g_last_watchdog_ms = 0;
    watchdog(); assert(!g_apply_pid && !g_work_pending);
    g_observed_ms -= 16000; g_last_watchdog_ms = 0;
    watchdog(); assert(!g_apply_pid && !g_work_pending);
    g_observed_ms = now_ms(); g_observed.ap.state = AP_UNKNOWN; g_last_watchdog_ms = 0;
    watchdog(); assert(!g_apply_pid && !g_work_pending);
    g_cfg.ssh_enabled = false;
    g_observed_ms = 0; g_ap_unready_ms = 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    const char *base = strrchr(argv[0], '/');
    if (!strcmp(base ? base + 1 : argv[0], "connmanctl")) return fake_connmanctl();
    if (!strcmp(base ? base + 1 : argv[0], "supplicantmanager")) { for (;;) pause(); }
    mkdir(APP_DIR, 0700);
    config_defaults();
    g_log_stderr = false;
    signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    test_command = mock_command;
    pid_t oem = fork();
    if (oem == 0) { execlp("supplicantmanager", "supplicantmanager", (char *)NULL); _exit(127); }
    assert(oem > 0);
    for (int i = 0; i < 100 && pid_by_executable("supplicantmanager") != oem; i++) wait_ms(5);
    assert(pid_by_executable("supplicantmanager") == oem);
    assert(pid_by_comm("supplicantmanager") < 0); /* real kernel truncation */
    stop_oem_wifi(); waitpid(oem, NULL, 0);

    char service[256];
    str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), "Lab");
    str_copy(g_cfg.psk, sizeof(g_cfg.psk), "fixture-password");
    assert(!find_service_psk("*AR Lab-Guest wifi_a_4c61622d4775657374_managed_psk\n", "Lab", "_psk", service, sizeof(service)));
    assert(find_service_psk("*AR Lab wifi_a_4c6162_managed_psk\n", "Lab", "_psk", service, sizeof(service)));
    assert(find_service_psk("    <hidden> wifi_a_4c6162_managed_psk\n", "Lab", "_psk", service, sizeof(service)));
    assert(!find_service_psk("*AR Lab wifi_bad;invalid\n", "Lab", "_psk", service, sizeof(service)));

    /* Multi-network selection: highest-priority visible network wins. */
    {
        config_t saved = g_cfg;
        char out[2048];
        memset(&g_cfg, 0, sizeof(g_cfg));
        str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), "Primary");
        str_copy(g_cfg.psk, sizeof(g_cfg.psk), "primary-password");
        str_copy(g_cfg.net2_ssid, sizeof(g_cfg.net2_ssid), "Backup");
        str_copy(g_cfg.net2_psk, sizeof(g_cfg.net2_psk), "backup-password");
        snprintf(out, sizeof(out),
                 "*AR Backup wifi_a_4261636b7570_managed_psk\n"
                 "*AR Primary wifi_a_5072696d617279_managed_psk\n");
        int net = find_service_any(out, service, sizeof(service), NULL, 0);
        assert(net == 0); /* Primary (index 0, higher priority) visible */
        snprintf(out, sizeof(out),
                 "*AR Backup wifi_a_4261636b7570_managed_psk\n");
        net = find_service_any(out, service, sizeof(service), NULL, 0);
        assert(net == 1); /* only Backup visible */
        station_status = "wpa_state=COMPLETED\nmode=station\nssid=Backup\n";
        assert(client_associated() && client_station_net() == 1);
        assert(find_connected_service("*AR Primary wifi_a_5072696d617279_managed_psk\n*AR Backup wifi_a_4261636b7570_managed_psk\n",
                                      service, sizeof(service)));
        assert(strstr(service, "4261636b7570"));
        g_client_net = 1;
        agent_state_t backup_agent = {.registered = true};
        char reply[256];
        assert(agent_reply(&backup_agent, "Passphrase?", service, reply, sizeof(reply)));
        assert(!strcmp(reply, "backup-password\n"));
        g_client_net = 0;
        station_status = NULL;
        snprintf(out, sizeof(out),
                 "*AR Elsewhere wifi_a_456c73657768657265_managed_psk\n");
        net = find_service_any(out, service, sizeof(service), NULL, 0);
        assert(net == -1); /* nothing configured is visible */
        assert(net_total() == 2);
        /* open-network security suffix follows the per-network psk */
        memset(&g_cfg, 0, sizeof(g_cfg));
        str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), "Open");
        snprintf(out, sizeof(out), "*AR Open wifi_a_4f70656e_managed_none\n");
        assert(find_service_psk(out, "Open", "_none", service, sizeof(service)));
        assert(!find_service_psk(out, "Open", "_psk", service, sizeof(service)));
        assert(net_total() == 1);
        g_cfg = saved;
    }

    station_status = "wpa_state=COMPLETED\nmode=station\nssid=Other\n";
    assert(!client_associated());
    station_status = "wpa_state=COMPLETED\nmode=AP\nssid=Lab\n";
    assert(!client_associated());
    station_status = "wpa_state=COMPLETED\nmode=station\nssid=Lab\n";
    assert(client_associated());
    char decoded[80];
    assert(decode_control_ssid(decoded, sizeof(decoded), "Lab\\\\Name\\x20WiFi") && !strcmp(decoded, "Lab\\Name WiFi"));
    assert(!decode_control_ssid(decoded, sizeof(decoded), "Lab\\x00bad"));
    test_iface = test_owned_wpa = 1; powered = false;
    assert(!ensure_wifi_ready()); /* PONG does not mean Powered=True */
    powered = true;
    assert(ensure_wifi_ready());
    service_reply = "State = ready\nIPv4 = [ Method=dhcp, Address=192.0.2.10, Netmask=255.255.255.0 ]\n";
    assert(client_service_ready("wifi_fixture", "192.0.2.10"));
    assert(!client_service_ready("wifi_fixture", "192.0.2.1"));
    service_reply = "State = configuration\nIPv4 = [ Address=192.0.2.10 ]\n";
    assert(!client_service_ready("wifi_fixture", "192.0.2.10"));
    station_status = NULL;
    fail_ap_power = true; ap_requests = 0;
    str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), "fixture-ap-key");
    assert(!ap_start() && ap_requests == 1); /* no fallback while radio remains unpowered */
    fail_ap_power = false; powered = true;
    success_request = 2; ap_requests = 0;
    assert(ap_start() && ap_requests == 2 && g_cfg.cp_band == 1 && g_cfg.cp_channel == 6);
    success_request = 0; ap_active = false;
    assert(fw_open(2224)); fw_close(2224); carplay_fw_open(); carplay_fw_close();
    g_cfg.mode = MODE_OFF; g_cfg.wlan_on = false;
    assert(!apply_mode_work(false)); /* mocked radio still alive: do not claim Off */
    test_owned_wpa = 0; powered = false;
    assert(apply_mode_work(false));
    test_owned_wpa = 1; powered = true;
    test_ap_states();

    agent_state_t agent = {0};
    char reply[512];
    str_copy(g_cfg.psk, sizeof(g_cfg.psk), "fixture-password");
    assert(agent_reply(&agent, "idle", "wifi_fixture", reply, sizeof(reply)) && !reply[0]);
    assert(agent_reply(&agent, "Agent registered", "wifi_fixture", reply, sizeof(reply)));
    assert(!strcmp(reply, "connect wifi_fixture\n"));
    assert(agent_reply(&agent, "waiting for asynchronous callback", "wifi_fixture", reply, sizeof(reply)) && !reply[0]);
    assert(agent_reply(&agent, "Passphrase? ", "wifi_fixture", reply, sizeof(reply)));
    assert(!strcmp(reply, "fixture-password\n"));
    assert(!agent_reply(&agent, "Passphrase? ", "wifi_fixture", reply, sizeof(reply)));
    agent = (agent_state_t){.registered = true};
    assert(!agent_reply(&agent, "Agent request cancelled", "wifi_fixture", reply, sizeof(reply)) && !reply[0]);
    assert(client_connman_agent("wifi_fixture")); /* real pipes, fragmented delayed prompt */

    g_apply_gen = 10; g_work_kind = WORK_MODE;
    g_apply_failed = true; g_carplay_ie = false;
    str_copy(g_last_msg, sizeof(g_last_msg), "current intent");
    char stale[] = "9 1\nobsolete success\ncarplay_ie=1\nssh_port=2229\n";
    assert(!accept_apply_result(stale));
    assert(g_apply_failed && !g_carplay_ie && !strcmp(g_last_msg, "current intent"));
    char malformed[] = "10\ninvalid\ncarplay_ie=1\n";
    assert(!accept_apply_result(malformed));
    char fresh[] = "10 1\nready\ncarplay_ie=1\ncp_id=fixture-id\nssh_fallback=1\nssh_active_port=2224\n";
    assert(accept_apply_result(fresh));
    assert(g_carplay_ie && g_ssh_port_fallback && g_ssh_active_port == 2224);
    assert(!strcmp(g_cfg.cp_id, "fixture-id"));
    g_cfg.mode = MODE_CARPLAY; g_cfg.wlan_on = true;
    g_observed.gen = g_apply_gen; g_observed_ms = now_ms();
    g_observed.ap_on = g_observed.carplay_svc = g_observed.mdns_svc = true;
    char status[8192];
    commands = 0; build_status(status, sizeof(status));
    assert(commands == 0 && strstr(status, "\"carplay_ready\":true"));
    g_observed_ms -= 16000;
    build_status(status, sizeof(status));
    assert(strstr(status, "\"carplay_ready\":false") && strstr(status, "\"observations_fresh\":false"));
    g_observed_ms = now_ms();

    test_work = slow_work;
    g_cfg.ssh_enabled = true;
    apply_mode(false);
    wait_ms(40);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pid_t writer = fork();
    if (writer == 0) {
        close(sockets[1]);
        write(sockets[0], "GET /hea", 8); wait_ms(30);
        const char *rest = "lth HTTP/1.1\r\nHost: localhost\r\n\r\n";
        write(sockets[0], rest, strlen(rest)); _exit(0);
    }
    int64_t health_start = now_ms(); http_handle(sockets[1]);
    assert(now_ms() - health_start < 100);
    char response[512]; ssize_t got = read(sockets[0], response, sizeof(response) - 1);
    assert(got > 0); response[got] = 0;
    assert(strstr(response, "200 OK")); close(sockets[0]);
    waitpid(writer, NULL, 0);
    int home_before = g_home_seq;
    home_monitor_line("method call member=NotifyRequestHomeScreen");
    assert(g_home_seq == home_before + 1);
    g_cfg.ssh_enabled = false;
    int64_t start = now_ms(); request_work(WORK_SSH, false);
    assert(now_ms() - start < 100); /* cancellation does not block the parent */
    pump();
    char effective[8]; assert(read_file(APP_DIR "/effective-ssh", effective, sizeof(effective)) > 0);
    assert(!strcmp(effective, "0") && !g_cfg.ssh_enabled && !g_apply_failed);
    g_cfg.mode = MODE_OFF; g_cfg.wlan_on = false;
    apply_mode(false); pump();
    assert(!g_carplay_ie);
    request_work(WORK_SCAN, false);
    wait_ms(20);
    request_work(WORK_SSH, false);
    assert(g_pending_kind == WORK_MODE); /* cancelled off-mode scan must power down */
    pump();
    test_work = NULL;

    assert(is_pruneable_name("wifi_logs_20261109010203"));
    assert(!is_pruneable_name("wifi_logs_notes"));
    assert(!is_pruneable_name("wifi_logs_20261109010203_extra"));
    char usb[512]; snprintf(usb, sizeof(usb), APP_DIR "/usb"); mkdir(usb, 0700);
    for (int i = 0; i < 25; i++) {
        char dir[600]; snprintf(dir, sizeof(dir), "%s/wifi_logs_20260101%06d", usb, i); mkdir(dir, 0700);
    }
    char outside[600], linkpath[700];
    snprintf(outside, sizeof(outside), APP_DIR "/outside");
    mkdir(outside, 0700);
    snprintf(linkpath, sizeof(linkpath), "%s/wifi_logs_20260101000000/linked", usb);
    assert(symlink(outside, linkpath) == 0);
    char unrelated[600]; snprintf(unrelated, sizeof(unrelated), "%s/wifi_logs_notes", usb);
    mkdir(unrelated, 0700);
    prune_old_dumps(usb);
    assert(file_exists(unrelated));
    assert(file_exists(outside));
    DIR *d = opendir(usb); struct dirent *de; int bundles = 0;
    while ((de = readdir(d))) if (is_pruneable_name(de->d_name)) bundles++;
    closedir(d); assert(bundles == 5);
    write_file_atomic(LOG_PATH, "2026 msg: harmless event\n2026 AP stage=enabled_then_lost role=0\n2026 msg: set_network 0 psk historical-secret\nconnmanctl> old-secret\nTetheringPassphrase = historical-ap-secret\n", 0600);
    copy_event_log(usb, LOG_PATH);
    char exported[8192], logpath[600]; snprintf(logpath, sizeof(logpath), "%s/daemon.log", usb);
    assert(read_file(logpath, exported, sizeof(exported)) > 0);
    assert(strstr(exported, "harmless event") && strstr(exported, "AP stage=enabled_then_lost") && !strstr(exported, "secret"));
    log_msg("fixture credential must be masked: %s", g_cfg.psk);
    assert(read_file(LOG_PATH, exported, sizeof(exported)) > 0 && !strstr(exported, g_cfg.psk));
    str_copy(g_cfg.net2_psk, sizeof(g_cfg.net2_psk), "backup-fixture-password");
    str_copy(g_cfg.net3_psk, sizeof(g_cfg.net3_psk), "third-fixture-password");
    str_copy(g_cfg.net4_psk, sizeof(g_cfg.net4_psk), "fourth-fixture-password");
    assert(config_save());
    copy_config_redacted(usb);
    char configpath[600]; snprintf(configpath, sizeof(configpath), "%s/config.conf", usb);
    assert(read_file(configpath, exported, sizeof(exported)) > 0);
    assert(!strstr(exported, "fixture-password"));
    assert(strstr(exported, "net2_psk=<redacted>") && strstr(exported, "net4_psk=<redacted>"));
    log_msg("secondary credential must be masked: %s", g_cfg.net2_psk);
    assert(read_file(LOG_PATH, exported, sizeof(exported)) > 0 && !strstr(exported, g_cfg.net2_psk));
    assert(write_file_atomic(STATUS_PATH,
        "{\"ok\":true,\"ap_active\":true,\"carplay_psk\":\"historic-not-current\",\"message\":\"historical-secret\"}", 0600) == 0);
    copy_status_redacted(usb);
    char statuspath[600]; snprintf(statuspath, sizeof(statuspath), "%s/status.json", usb);
    assert(read_file(statuspath, exported, sizeof(exported)) > 0);
    assert(strstr(exported, "\"ap_active\":true") && !strstr(exported, "historic"));
    setenv("WIFI_USB_ROOT", usb, 1);
    saw_ap_scan = saw_iwconfig = false;
    assert(dump_logs_to_usb(true));
    assert(saw_ap_scan && saw_iwconfig);
    struct stat before_log, after_log;
    assert(stat(LOG_PATH, &before_log) == 0);
    g_pure_query = true;
    log_msg("pure query must not append or create a log");
    g_pure_query = false;
    assert(stat(LOG_PATH, &after_log) == 0 && before_log.st_size == after_log.st_size);
    assert(rename(CONFIG_PATH, APP_DIR "/saved-config") == 0);
    assert(mkdir(CONFIG_PATH, 0700) == 0); /* deterministic atomic-rename failure */
    assert(write_file_atomic(APP_DIR "/import-fixture", "ssid=Different\npsk=new-fixture-key\n", 0600) == 0);
    char previous_ssid[80]; str_copy(previous_ssid, sizeof(previous_ssid), g_cfg.ssid);
    assert(!parse_wifi_txt(APP_DIR "/import-fixture"));
    assert(!strcmp(previous_ssid, g_cfg.ssid) && !g_cfg.wlan_on && !g_cfg.ssh_enabled);
    assert(rmdir(CONFIG_PATH) == 0 && rename(APP_DIR "/saved-config", CONFIG_PATH) == 0);

    test_command = NULL;
    start = now_ms();
    assert(run_command(NULL, 0, "sleep 10", 120) == 124);
    assert(now_ms() - start < 1000);
    puts("PASS: behavioral C regressions (agent, ownership, power, results, cancellation, privacy, pruning, deadlines)");
    return 0;
}
