/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Orange Pi USB-SSH lifecycle, ownership, cleanup and key fixtures.
 * The command fixture models the live kernel/firewall state and mutates it
 * only for exact canonical commands, so missing post-delete readbacks and
 * success-without-effect faults are visible.  No host network is touched.
 */
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main

static bool g_live_addr;
static bool g_live_default;
static bool g_chain_present;
static bool g_chain_hook;
static int g_chain_port;
static const char *g_chain_override;
static const char *g_hook_override;
static bool g_fail_ip_query;
static bool g_fail_fw_query;
static bool g_fail_addr_add;
static bool g_fail_addr_del;
static bool g_fail_chain;
static bool g_fail_hook;
static bool g_fail_rule;
static bool g_fault_no_effect;
static int g_full_capture;

static void live_reset(void)
{
    g_live_addr = false;
    g_live_default = false;
    g_chain_present = false;
    g_chain_hook = false;
    g_chain_port = 0;
    g_chain_override = NULL;
    g_hook_override = NULL;
    g_fail_ip_query = false;
    g_fail_fw_query = false;
    g_fail_addr_add = false;
    g_fail_addr_del = false;
    g_fail_chain = false;
    g_fail_hook = false;
    g_fail_rule = false;
    g_fault_no_effect = false;
}

static int write_bytes(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int write_marker(const char *path)
{
    return write_bytes(path, "");
}

static void self_comm(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen("/proc/self/comm", "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = 0;
    }
    fclose(f);
}

static bool file_text_is(const char *path, const char *text)
{
    char buf[4096];
    int n = read_file(path, buf, sizeof(buf));
    return n == (int)strlen(text) && !memcmp(buf, text, strlen(text));
}

static void remove_usb0_files(void)
{
    unlink(USB0_HELPER);
    unlink(USB0_RULE_PATH);
    unlink(USB0_OWNER);
}

static void append(char *out, size_t cap, const char *text)
{
    size_t used = strlen(out);
    if (used + strlen(text) < cap)
        memcpy(out + used, text, strlen(text) + 1);
}

static int fixture_command(char *out, size_t n, const char *cmd)
{
    if (out && n) out[0] = 0;
    if (g_full_capture) {
        /* Simulate a successful but larger-than-capture table. The decisive
         * modem/default/rule may be after the returned prefix. */
        if (g_full_capture == 1 &&
            (strstr(cmd, "addr show dev rmnet") || strstr(cmd, "ifconfig rmnet")))
            return 1;
        if (out && n) { memset(out, ' ', n - 1); out[n - 1] = 0; }
        return 0;
    }
    if (strstr(cmd, "dropbearmulti") != NULL)
        return system(cmd);
    if (strstr(cmd, "chsmack") != NULL || strstr(cmd, "udevadm") != NULL)
        return 0;
    if (strstr(cmd, "ip -o -4 addr show") != NULL) {
        if (g_fail_ip_query) return 1;
        bool scoped = strstr(cmd, "dev usb0") != NULL;
        if (!scoped && out && n)
            snprintf(out, n, "2: wlan0    inet 10.42.0.2/24 scope global wlan0\n");
        if (g_live_addr && out && n) {
            char line[128];
            snprintf(line, sizeof(line),
                     "5: usb0    inet " WIFI1B_USB0_IP "/24 scope global usb0\n");
            append(out, n, line);
        }
        return 0;
    }
    if (strstr(cmd, "ip -4 route show default") != NULL) {
        if (g_fail_ip_query) return 1;
        if (g_live_default && out && n)
            snprintf(out, n, "default via 10.55.0.2 dev usb0 metric 0\n");
        return 0;
    }
    if (strstr(cmd, "ip -4 addr add") != NULL) {
        if (g_fail_addr_add) return 1;
        if (!g_fault_no_effect) g_live_addr = true;
        return 0;
    }
    if (strstr(cmd, "ip -4 addr del") != NULL) {
        if (g_fail_addr_del) return 1;
        if (!g_fault_no_effect) g_live_addr = false;
        return 0;
    }
    if (strstr(cmd, "ip link set") != NULL) return 0;
    if (strstr(cmd, "iptables") != NULL) {
        bool scoped = strstr(cmd, "-S WIFI1B_USB0_SSH") != NULL;
        bool input = strstr(cmd, "-S INPUT") != NULL;
        bool all = !scoped && !input && strstr(cmd, " -S ") != NULL;
        if (scoped || all) {
            if (g_fail_fw_query) return 1;
            if (scoped && !g_chain_present && g_chain_override == NULL)
                return 1;
            if (g_chain_override != NULL && out && n)
                snprintf(out, n, "%s", g_chain_override);
            else if (g_chain_present && out && n) {
                append(out, n, "-N WIFI1B_USB0_SSH\n");
                if (g_chain_port > 0) {
                    char rule[160];
                    snprintf(rule, sizeof(rule),
                             "-A WIFI1B_USB0_SSH -p tcp -m tcp --dport %d "
                             "-j ACCEPT\n", g_chain_port);
                    append(out, n, rule);
                }
            }
            if (all && out && n) {
                if (g_hook_override != NULL)
                    append(out, n, g_hook_override);
                else if (g_chain_hook)
                    append(out, n,
                           "-A INPUT -i usb0 -j WIFI1B_USB0_SSH\n");
            }
            return 0;
        }
        if (input) {
            if (g_fail_fw_query) return 1;
            if (out && n) {
                if (g_hook_override != NULL)
                    snprintf(out, n, "%s", g_hook_override);
                else if (g_chain_hook)
                    snprintf(out, n,
                             "-A INPUT -i usb0 -j WIFI1B_USB0_SSH\n");
            }
            return 0;
        }
        if (strstr(cmd, "-N WIFI1B_USB0_SSH") != NULL) {
            if (g_fail_chain) return 1;
            if (!g_fault_no_effect) g_chain_present = true;
            return 0;
        }
        if (strstr(cmd, "-I INPUT") != NULL) {
            if (g_fail_hook) return 1;
            if (!g_fault_no_effect) g_chain_hook = true;
            return 0;
        }
        if (strstr(cmd, "-D INPUT") != NULL) {
            if (!g_fault_no_effect) g_chain_hook = false;
            return 0;
        }
        if (strstr(cmd, "-A WIFI1B_USB0_SSH") != NULL) {
            if (g_fail_rule) return 1;
            const char *dport = strstr(cmd, "--dport ");
            if (!g_fault_no_effect && dport != NULL)
                g_chain_port = atoi(dport + 8);
            return 0;
        }
        if (strstr(cmd, "-D WIFI1B_USB0_SSH") != NULL) {
            if (!g_fault_no_effect) g_chain_port = 0;
            return 0;
        }
        if (strstr(cmd, "-X WIFI1B_USB0_SSH") != NULL) {
            if (!g_fault_no_effect && g_chain_port == 0 && !g_chain_hook)
                g_chain_present = false;
            return 0;
        }
        return 0;
    }
    return 0;
}

static int test_lifecycle(void)
{
    live_reset();
    remove_usb0_files();
    unlink(WIFI1B_USB0_SYSFS);
    if (ssh_start() != true) return 2;
    if (owned_dropbear_pid() > 0) return 3;
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 4;
    live_reset();
    if (ssh_start() != true) return 5;
    if (owned_dropbear_pid() <= 0) return 6;
    ssh_stop();
    if (owned_dropbear_pid() > 0) return 7;
    g_cfg.ssh_enabled = false;
    if (ssh_start() != false) return 8;
    g_cfg.ssh_enabled = true;
    return 0;
}

static int test_home_only_key(void)
{
    mkdir(SSH_HOME, 0700);
    mkdir(SSH_HOME "/.ssh", 0700);
    unlink(SSH_KEYS);
    unlink(SSH_HOME "/.ssh/authorized_keys");
    if (write_bytes(SSH_HOME "/.ssh/authorized_keys", "owner-home-key\n") != 0)
        return 10;
    live_reset();
    if (ssh_start() != true) return 11;
    if (!file_text_is(SSH_HOME "/.ssh/authorized_keys", "owner-home-key\n"))
        return 12;
    ssh_stop();
    return 0;
}

static int test_shutdown_and_wifi_failure(void)
{
    g_shutdown_cleanup = true;
    g_stop = false;
    g_cfg.ssh_enabled = true;
    ssh_after_wifi_failure();
    if (owned_dropbear_pid() > 0) return 20;
    g_shutdown_cleanup = false;
    live_reset();
    if (ssh_start() != true) return 21;
    if (owned_dropbear_pid() <= 0) return 22;
    ssh_after_wifi_failure();
    if (owned_dropbear_pid() <= 0) return 23;
    ssh_stop();
    return 0;
}

static int test_hostile_helper_destinations(void)
{
    char victim[512], tmp[512];
    snprintf(victim, sizeof(victim), "%s/owner-helper-victim", APP_DIR);
    remove_usb0_files();
    unlink(WIFI1B_USB0_SYSFS);
    live_reset();
    if (write_bytes(victim, "OWNER-HELPER\n") != 0) return 30;
    if (symlink(victim, USB0_HELPER) != 0) return 31;
    if (usb0_rule_write(2224)) return 32;
    if (!file_text_is(victim, "OWNER-HELPER\n")) return 33;
    unlink(USB0_HELPER);
    unlink(victim);

    remove_usb0_files();
    if (mkdir(USB0_HELPER, 0700) != 0) return 34;
    if (usb0_rule_write(2224)) return 35;
    {
        struct stat st;
        if (lstat(USB0_HELPER, &st) != 0 || !S_ISDIR(st.st_mode)) return 36;
    }
    if (rmdir(USB0_HELPER) != 0) return 37;

    remove_usb0_files();
    if (mkfifo(USB0_HELPER, 0600) != 0) return 38;
    if (usb0_rule_write(2224)) return 39;
    {
        struct stat st;
        if (lstat(USB0_HELPER, &st) != 0 || !S_ISFIFO(st.st_mode)) return 40;
    }
    unlink(USB0_HELPER);

    remove_usb0_files();
    if (write_bytes(victim, "OWNER-HARDLINK\n") != 0) return 41;
    if (link(victim, USB0_HELPER) != 0) return 42;
    if (usb0_rule_write(2224)) return 43;
    if (!file_text_is(victim, "OWNER-HARDLINK\n")) return 44;
    {
        struct stat st;
        if (lstat(USB0_HELPER, &st) != 0 || st.st_nlink != 2) return 45;
    }
    unlink(USB0_HELPER);
    unlink(victim);

    /* Any pre-existing regular temp is owner data, not a stale temp. */
    remove_usb0_files();
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", USB0_HELPER, (int)getpid());
    if (write_bytes(tmp, "OWNER-TEMP-REGULAR\n") != 0) return 46;
    if (write_file_atomic(USB0_HELPER, "x\n", 0644) == 0) return 47;
    if (!file_text_is(tmp, "OWNER-TEMP-REGULAR\n")) return 48;
    unlink(tmp);

    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", USB0_HELPER, (int)getpid());
    if (write_bytes(victim, "OWNER-TEMP-LINK\n") != 0) return 49;
    if (symlink(victim, tmp) != 0) return 50;
    if (write_file_atomic(USB0_HELPER, "x\n", 0644) == 0) return 51;
    if (!file_text_is(victim, "OWNER-TEMP-LINK\n")) return 52;
    unlink(tmp);
    unlink(victim);
    return 0;
}

static int test_hostile_rule_destination(void)
{
    char victim[512];
    snprintf(victim, sizeof(victim), "%s/owner-rule-victim", APP_DIR);
    remove_usb0_files();
    unlink(WIFI1B_USB0_SYSFS);
    live_reset();
    if (write_bytes(victim, "OWNER-RULE\n") != 0) return 60;
    if (symlink(victim, USB0_RULE_PATH) != 0) return 61;
    if (usb0_rule_write(2224)) return 62;
    if (!file_text_is(victim, "OWNER-RULE\n")) return 63;
    unlink(USB0_RULE_PATH);
    unlink(victim);
    remove_usb0_files();
    return 0;
}

static int test_conflicting_key_destinations(void)
{
    char victim[512], eff[512];
    snprintf(victim, sizeof(victim), "%s/owner-key-victim", APP_DIR);
    snprintf(eff, sizeof(eff), SSH_HOME "/.ssh/authorized_keys");
    ssh_stop();
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 69;
    mkdir(SSH_DIR, 0700);
    mkdir(SSH_HOME "/.ssh", 0700);
    unlink(eff);
    if (write_bytes(SSH_KEYS,
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAICanonicalOne canonical-one\n"
        "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCanonicalTwo canonical-two\n") != 0)
        return 70;
    if (write_bytes(eff,
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAICanonicalOne canonical-one\n"
        "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCanonicalTwo canonical-two\n"
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOwnerTermius owner-termius\n") != 0)
        return 71;
    live_reset();
    if (ssh_start() != false) return 72;
    {
        char home[4096] = "";
        (void)read_file(eff, home, sizeof(home));
        if (strstr(home, "owner-termius") == NULL) return 73;
    }
    if (owned_dropbear_pid() > 0) return 74;

    unlink(eff);
    if (write_bytes(victim, "OWNER-KEY\n") != 0) return 75;
    if (symlink(victim, eff) != 0) return 76;
    if (ssh_start() != false) return 77;
    if (!file_text_is(victim, "OWNER-KEY\n")) return 78;
    unlink(eff);
    unlink(victim);
    unlink(SSH_KEYS);
    return 0;
}

static int test_explicit_key_pair_import(void)
{
    char eff[512];
    snprintf(eff, sizeof(eff), SSH_HOME "/.ssh/authorized_keys");
    mkdir(SSH_DIR, 0700);
    mkdir(SSH_HOME "/.ssh", 0700);
    unlink(SSH_KEYS);
    unlink(eff);
    if (write_bytes(eff, "owner-termius\n") != 0) return 80;
    if (!write_key_pair_exact("new-key\n")) return 81;
    if (!file_text_is(SSH_KEYS, "new-key\n") ||
        !file_text_is(eff, "new-key\n")) return 82;

    unlink(SSH_KEYS);
    if (write_bytes(eff, "owner-termius\n") != 0) return 83;
    char fifo[600];
    snprintf(fifo, sizeof(fifo), "%s.unsafe", eff);
    unlink(fifo);
    if (mkfifo(fifo, 0600) != 0) return 84;
    unlink(eff);
    if (rename(fifo, eff) != 0) return 85;
    if (write_key_pair_exact("new-key\n")) return 86;
    if (access(SSH_KEYS, F_OK) == 0) return 87;
    unlink(eff);
    unlink(SSH_KEYS);

    /* A second-destination failure rolls the canonical write back. */
    mkdir(SSH_HOME "/.ssh", 0700);
    unlink(eff);
    unlink(SSH_KEYS);
    if (chmod(SSH_HOME "/.ssh", 0500) != 0) return 88;
    bool pair_ok = write_key_pair_exact("rollback-key\n");
    (void)chmod(SSH_HOME "/.ssh", 0700);
    if (pair_ok) return 89;
    if (access(SSH_KEYS, F_OK) == 0) return 196;
    return 0;
}

static int test_cleanup_idempotent_absent(void)
{
    remove_usb0_files();
    live_reset();
    unlink(WIFI1B_USB0_SYSFS);
    if (write_bytes(SSH_BOUND, "owned-bound-marker\n") != 0) return 90;
    if (!usb0_cleanup_owned()) return 91;
    if (access(SSH_BOUND, F_OK) != 0) return 92;
    unlink(SSH_BOUND);
    return 0;
}

static int test_cleanup_no_owner_preserves(void)
{
    remove_usb0_files();
    live_reset();
    g_live_addr = true;
    g_chain_present = true;
    g_chain_hook = true;
    g_chain_port = 2224;
    if (write_bytes(SSH_BOUND, "owned-bound-marker\n") != 0) return 100;
    if (usb0_cleanup_owned()) return 101;
    if (!g_live_addr || !g_chain_present || !g_chain_hook ||
        g_chain_port != 2224) return 102;
    if (access(SSH_BOUND, F_OK) != 0) return 103;
    unlink(SSH_BOUND);
    live_reset();
    return 0;
}

static int test_cleanup_unknown_preserves(void)
{
    remove_usb0_files();
    live_reset();
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 110;
    if (!usb0_rule_write(2224)) return 111;
    g_fail_ip_query = true;
    if (usb0_cleanup_owned()) return 112;
    if (!file_exists(USB0_HELPER) || !file_exists(USB0_RULE_PATH) ||
        !file_exists(USB0_OWNER)) return 113;
    g_fail_ip_query = false;
    g_fail_fw_query = true;
    if (usb0_cleanup_owned()) return 114;
    if (!file_exists(USB0_HELPER) || !file_exists(USB0_RULE_PATH) ||
        !file_exists(USB0_OWNER)) return 115;
    g_fail_fw_query = false;
    remove_usb0_files();
    live_reset();
    return 0;
}

static int test_cleanup_exact(void)
{
    remove_usb0_files();
    live_reset();
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 120;
    if (!usb0_rule_write(2224)) return 121;
    if (!usb0_cleanup_owned()) return 122;
    if (g_live_addr || g_chain_present || g_chain_hook || g_chain_port != 0)
        return 123;
    if (file_exists(USB0_HELPER) || file_exists(USB0_RULE_PATH) ||
        file_exists(USB0_OWNER)) return 124;
    return 0;
}

static int test_cleanup_fault_no_effect(void)
{
    remove_usb0_files();
    live_reset();
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 130;
    if (!usb0_rule_write(2224)) return 131;
    g_fault_no_effect = true;
    if (usb0_cleanup_owned()) return 132;
    if (!g_live_addr || !g_chain_present || !g_chain_hook ||
        g_chain_port != 2224) return 133;
    if (!file_exists(USB0_HELPER) || !file_exists(USB0_RULE_PATH) ||
        !file_exists(USB0_OWNER)) return 134;
    g_fault_no_effect = false;
    remove_usb0_files();
    live_reset();
    return 0;
}

static int test_apply_exact(void)
{
    remove_usb0_files();
    live_reset();
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 140;
    if (!usb0_rule_write(2224)) return 141;
    if (!g_live_addr || !g_chain_present || !g_chain_hook ||
        g_chain_port != 2224) return 142;
    /* A canonical rule for an unexpected port is not silently replaced by
     * the standalone apply mutator. */
    g_chain_port = 9999;
    if (usb0_apply_owned(2224)) return 148;
    if (g_chain_port != 9999) return 149;
    g_chain_port = 2224;
    if (!usb0_rule_write(2225)) return 143;
    if (g_chain_port != 2225) return 144;
    if (!usb0_rule_write(0)) return 145;
    if (g_chain_port != 0 || !g_chain_present || !g_chain_hook) return 146;
    if (!g_live_addr) return 147;
    /* The udev one-shot mutator requires the exact owned files for its port. */
    if (!usb0_apply_owned(0)) return 148;
    return 0;
}

static int test_unknown_regular_preserved(void)
{
    remove_usb0_files();
    live_reset();
    unlink(WIFI1B_USB0_SYSFS);
    if (write_bytes(USB0_HELPER, "OWNER-HELPER-REGULAR\n") != 0 ||
        write_bytes(USB0_RULE_PATH, "OWNER-RULE-REGULAR\n") != 0 ||
        write_bytes(USB0_OWNER, "OWNER-OWNER-REGULAR\n") != 0) return 150;
    if (usb0_rule_write(2224)) return 151;
    if (!file_text_is(USB0_HELPER, "OWNER-HELPER-REGULAR\n") ||
        !file_text_is(USB0_RULE_PATH, "OWNER-RULE-REGULAR\n") ||
        !file_text_is(USB0_OWNER, "OWNER-OWNER-REGULAR\n")) return 152;

    remove_usb0_files();
    unlink(WIFI1B_USB0_SYSFS);
    live_reset();
    if (!usb0_rule_write(2224)) return 153;
    if (write_bytes(USB0_OWNER, "OWNER-OWNER-DATA\n") != 0) return 154;
    if (usb0_rule_write(2224)) return 155;
    if (!file_text_is(USB0_OWNER, "OWNER-OWNER-DATA\n")) return 156;

    remove_usb0_files();
    live_reset();
    if (!usb0_rule_write(2224)) return 157;
    if (write_bytes(USB0_RULE_PATH, "OWNER-RULE-DATA\n") != 0) return 158;
    if (usb0_rule_write(2224)) return 159;
    if (!file_text_is(USB0_RULE_PATH, "OWNER-RULE-DATA\n")) return 160;
    remove_usb0_files();
    return 0;
}

static int test_known_port_update(void)
{
    remove_usb0_files();
    live_reset();
    unlink(WIFI1B_USB0_SYSFS);
    if (!usb0_rule_write(2224)) return 160;
    g_live_addr = true;
    g_chain_present = true;
    g_chain_hook = true;
    g_chain_port = 2224;
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 161;
    if (!usb0_rule_write(2225)) return 162;
    if (g_chain_port != 2225 || !g_live_addr) return 163;
    {
        char owner[128];
        snprintf(owner, sizeof(owner),
                 "v1 scope=wifi1b-usb0 port=2225 address=" WIFI1B_USB0_IP "/24\n");
        if (!file_text_is(USB0_OWNER, owner)) return 164;
    }
    remove_usb0_files();
    live_reset();
    return 0;
}

static int test_legacy_helper_adoption(void)
{
    remove_usb0_files();
    live_reset();
    unlink(WIFI1B_USB0_SYSFS);
    if (write_bytes(USB0_HELPER, usb0_helper_legacy_text) != 0) return 170;
    {
        char rule[512];
        if (!usb0_rule_build(rule, sizeof(rule), 2224) ||
            write_bytes(USB0_RULE_PATH, rule) != 0) return 171;
    }
    g_live_addr = true;
    g_chain_present = true;
    g_chain_hook = true;
    g_chain_port = 2224;
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 172;
    if (!usb0_rule_write(2224)) return 173;
    if (file_text_is(USB0_HELPER, usb0_helper_legacy_text)) return 174;
    if (!file_exists(USB0_OWNER)) return 175;
    if (g_chain_port != 2224) return 176;
    remove_usb0_files();
    live_reset();
    return 0;
}

static int test_chain_parser_matrix(void)
{
    int port = 0;
    bool present = false, hook = false;
    char spec[512];
    remove_usb0_files();
    unlink(WIFI1B_USB0_SYSFS);
    live_reset();

    g_chain_override =
        "-N WIFI1B_USB0_SSH\n"
        "-A WIFI1B_USB0_SSH -p tcp -m tcp --dport 2224 -j ACCEPT\n";
    g_hook_override = "-A INPUT -i usb0 -j WIFI1B_USB0_SSH\n";
    if (usb0_chain_probe(2224, true, &port, &present, &hook, spec,
                         sizeof(spec)) != USB0_LIVE_OWNED) return 180;
    g_chain_override =
        "-N WIFI1B_USB0_SSH\n"
        "-A WIFI1B_USB0_SSH -p udp --dport 2224 -j ACCEPT\n";
    if (usb0_chain_probe(2224, true, &port, &present, &hook, spec,
                         sizeof(spec)) != USB0_LIVE_FOREIGN) return 181;
    g_chain_override =
        "-N WIFI1B_USB0_SSH\n"
        "-A WIFI1B_USB0_SSH -m conntrack --ctstate NEW -p tcp "
        "--dport 2224 -j ACCEPT\n";
    if (usb0_chain_probe(2224, true, &port, &present, &hook, spec,
                         sizeof(spec)) != USB0_LIVE_FOREIGN) return 182;
    g_chain_override =
        "-N WIFI1B_USB0_SSH\n"
        "-A WIFI1B_USB0_SSH -p tcp --dport 9999 -j ACCEPT\n";
    if (usb0_chain_probe(2224, false, &port, &present, &hook, spec,
                         sizeof(spec)) != USB0_LIVE_OWNED || port != 9999)
        return 183;
    g_chain_override =
        "-N WIFI1B_USB0_SSH\n"
        "-A WIFI1B_USB0_SSH -p tcp --dport 2224 -j ACCEPT\n"
        "-A WIFI1B_USB0_SSH -p tcp --dport 2224 -j ACCEPT\n";
    if (usb0_chain_probe(2224, false, &port, &present, &hook, spec,
                         sizeof(spec)) != USB0_LIVE_FOREIGN) return 184;
    g_chain_override = "-N WIFI1B_USB0_SSH\n";
    if (usb0_chain_probe(2224, true, &port, &present, &hook, spec,
                         sizeof(spec)) != USB0_LIVE_OWNED || port != 0)
        return 185;
    g_chain_override = NULL;
    g_hook_override = NULL;
    live_reset();
    return 0;
}

static int test_cleanup_foreign_chain(void)
{
    remove_usb0_files();
    live_reset();
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 190;
    if (!usb0_rule_write(2224)) return 191;
    g_chain_override =
        "-N WIFI1B_USB0_SSH\n"
        "-A WIFI1B_USB0_SSH -p udp --dport 2224 -j ACCEPT\n";
    if (usb0_cleanup_owned()) return 192;
    if (!file_exists(USB0_HELPER) || !file_exists(USB0_RULE_PATH) ||
        !file_exists(USB0_OWNER)) return 193;
    g_chain_override = NULL;
    remove_usb0_files();
    live_reset();
    return 0;
}

static int test_credential_parent_refusal(void)
{
    g_shutdown_cleanup = false;
    g_stop = false;
    g_cfg.ssh_enabled = true;
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 228;
    ssh_stop();
    char outside[512], real[512], eff[512];
    snprintf(outside, sizeof(outside), APP_DIR "/owner-key-directory");
    snprintf(real, sizeof(real), "%s/authorized_keys", outside);
    snprintf(eff, sizeof(eff), SSH_HOME "/.ssh/authorized_keys");
    if (mkdir(outside, 0700) != 0) return 201;
    if (write_bytes(real, "OWNER-KEY\n") != 0) return 202;
    if (rename(SSH_DIR, APP_DIR "/saved-ssh") != 0) return 203;
    if (symlink(outside, SSH_DIR) != 0) return 204;
    bool existed = false;
    char buf[256];
    if (w2_single_regular(SSH_KEYS, NULL) ||
        read_optional_regular(SSH_KEYS, buf, sizeof(buf), &existed) ||
        ssh_start() || write_key_pair_exact("replacement\n")) return 205;
    if (!file_text_is(real, "OWNER-KEY\n")) return 206;
    if (unlink(SSH_DIR) != 0 || rename(APP_DIR "/saved-ssh", SSH_DIR) != 0)
        return 207;
    if (rename(SSH_HOME, APP_DIR "/saved-home") != 0) return 208;
    if (symlink(outside, SSH_HOME) != 0) return 209;
    if (ssh_start() || write_key_pair_exact("replacement\n")) return 210;
    test_authorized_keys_source = real;
    bool imported = import_authorized_keys();
    test_authorized_keys_source = NULL;
    if (imported || !file_text_is(real, "OWNER-KEY\n")) return 232;
    char nested[512];
    snprintf(nested, sizeof(nested), "%s/.ssh", outside);
    if (access(nested, F_OK) == 0) return 211;
    if (unlink(SSH_HOME) != 0 || rename(APP_DIR "/saved-home", SSH_HOME) != 0)
        return 212;
    unlink(real);
    if (rmdir(outside) != 0) return 213;

    /* The second ENOENT must never hide the first EACCES. */
    unlink(eff);
    if (write_bytes(SSH_KEYS, "canonical\n") != 0) return 214;
    if (geteuid() != 0) {
        if (chmod(SSH_DIR, 0000) != 0) return 215;
        bool started = ssh_start();
        if (chmod(SSH_DIR, 0700) != 0) return 216;
        if (started || owned_dropbear_pid() > 0) return 217;
    }
    unlink(SSH_KEYS);
    /* Existing directories are not silently adopted or chmodded. */
    if (chmod(SSH_DIR, 0777) != 0) return 233;
    if (ssh_start()) return 234;
    struct stat st;
    if (lstat(SSH_DIR, &st) != 0 || (st.st_mode & 0777) != 0777) return 235;
    if (chmod(SSH_DIR, 0700) != 0) return 236;
    if (chmod(SSH_HOME "/.ssh", 0755) != 0) return 237;
    if (ssh_start()) return 238;
    if (lstat(SSH_HOME "/.ssh", &st) != 0 ||
        (st.st_mode & 0777) != 0755) return 239;
    if (chmod(SSH_HOME "/.ssh", 0700) != 0) return 240;
    return 0;
}

static int test_incomplete_readbacks(void)
{
    char out[8];
    test_command = NULL;
    if (run_command(out, sizeof(out), "printf '123456'", 1000) != 0 ||
        strcmp(out, "123456")) return 218;
    if (run_command(out, sizeof(out), "printf '1234567'", 1000) != 125 ||
        out[0]) return 219;
    if (run_command(out, sizeof(out), "printf '123456789012345'", 1000) != 125 ||
        out[0]) return 220;
    test_command = fixture_command;
    g_full_capture = 1;
    if (w2_iface_ipv4_state("rmnet_usb0") != W2_IPV4_UNKNOWN ||
        w2_modem_ipv4_state() != W2_IPV4_UNKNOWN) return 221;
    g_full_capture = 2;
    bool present = false, hook = false;
    int port = 0;
    char spec[512];
    if (usb0_addr_probe(&present) != USB0_LIVE_UNKNOWN ||
        usb0_default_probe() != USB0_LIVE_UNKNOWN ||
        usb0_chain_probe(-1, false, &port, &present, &hook, spec,
                          sizeof(spec)) != USB0_LIVE_UNKNOWN) return 222;
    if (w2_route_exact_present("10.42.0.1", "wlan0", 101, 66, &present))
        return 223;
    g_full_capture = 0;
    return 0;
}

static int test_full_owner_records(void)
{
    const char *token = "0123456789abcdef0123456789abcdef";
    char text[1024];
    w2_dhcp_lineage_t lineage;
    w2_owned_route_t route;
    w2_owner_journal_t journal;
    snprintf(text, sizeof(text), "v1 scope=wifi1b-dhcp lineage=%s high_water=1\n", token);
    if (write_bytes(WIFI1B_DHCP_LINEAGE, text) != 0 ||
        chmod(WIFI1B_DHCP_LINEAGE, 0600) != 0 ||
        w2_dhcp_lineage_load(&lineage) != 1) return 229;
    size_t used = strlen(text);
    memset(text + used, ' ', 255 - used);
    strcpy(text + 255, "FOREIGN-TAIL\n");
    if (write_bytes(WIFI1B_DHCP_LINEAGE, text) != 0 ||
        chmod(WIFI1B_DHCP_LINEAGE, 0600) != 0 ||
        w2_dhcp_lineage_load(&lineage) != -1) return 224;
    snprintf(text, sizeof(text), "v1 scope=wifi1b-dhcp lineage=%s high_water=1\n", token);
    if (write_bytes(WIFI1B_DHCP_LINEAGE, text) != 0) return 225;
    snprintf(text, sizeof(text), "v2 scope=wifi1b-dhcp-v1 lineage=%s gen=1 pid=%d gw=10.42.0.1 iface=wlan0 metric=101 proto=66\n", token, (int)getpid());
    if (write_bytes(WIFI1B_CLIENT_ROUTE_LEDGER, text) != 0 ||
        chmod(WIFI1B_CLIENT_ROUTE_LEDGER, 0600) != 0 ||
        w2_owned_route_load(&route) != 1) return 230;
    used = strlen(text);
    memset(text + used, ' ', 511 - used);
    strcpy(text + 511, "FOREIGN-TAIL\n");
    if (write_bytes(WIFI1B_CLIENT_ROUTE_LEDGER, text) != 0 ||
        chmod(WIFI1B_CLIENT_ROUTE_LEDGER, 0600) != 0 ||
        w2_owned_route_load(&route) != -1) return 226;
    snprintf(text, sizeof(text), "v2 scope=wifi1b-dhcp-v1 lineage=%s state=current gen=1 pid=%d iface=wlan0 ip=10.42.0.10 prefix=24\n", token, (int)getpid());
    if (write_bytes(WIFI1B_DHCP_OWNER_JOURNAL, text) != 0 ||
        chmod(WIFI1B_DHCP_OWNER_JOURNAL, 0600) != 0 ||
        w2_dhcp_journal_load(&journal) != 1) return 231;
    used = strlen(text);
    memset(text + used, ' ', 511 - used);
    strcpy(text + 511, "FOREIGN-TAIL\n");
    if (write_bytes(WIFI1B_DHCP_OWNER_JOURNAL, text) != 0 ||
        chmod(WIFI1B_DHCP_OWNER_JOURNAL, 0600) != 0 ||
        w2_dhcp_journal_load(&journal) != -1) return 227;
    unlink(WIFI1B_DHCP_LINEAGE);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    unlink(WIFI1B_DHCP_OWNER_JOURNAL);
    return 0;
}

/* Regression: a foreign wlan0 reservation must not block the usb0 SSH worker.
 * Dropbear and the USB firewall rule do not touch wlan0. */
static int test_ssh_worker_ignores_foreign_radio_owner(void)
{
    config_defaults();
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_cfg.ssh_enabled = true;
    g_cfg.ssh_port = 2224;
    test_command = fixture_command;
    live_reset();
    ssh_stop();
    if (owned_dropbear_pid() > 0) return 1;
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 2;

    g_work_kind = WORK_MODE;
    g_pending_kind = WORK_MODE;
    g_work_pending = false;
    g_apply_pid = 0;
    g_pairing_stop_requested = false;
    char comm[32] = {0};
    self_comm(comm, sizeof(comm));
    char owner[96];
    snprintf(owner, sizeof(owner), "%s:%d", comm, (int)getpid());
    if (write_bytes(RADIO_OWNER_PATH, owner) != 0) return 3;
    request_work(WORK_SSH, false);
    for (int i = 0; i < 60 && owned_dropbear_pid() <= 0; i++) usleep(50000);
    if (owned_dropbear_pid() <= 0) return 4;
    if (g_apply_pid > 0) {
        waitpid(g_apply_pid, NULL, 0);
        g_apply_pid = 0;
    }
    ssh_stop();
    unlink(RADIO_OWNER_PATH);
    unlink(WIFI1B_USB0_SYSFS);
    return 0;
}

/* Regression for the field failure: in WiFi client mode with the association
 * down, the WiFi health branches return early.  The USB-SSH ensure must run
 * before them so the Pi management link is restored without waiting for WiFi. */
static int test_watchdog_usb_before_wifi_early_return(void)
{
    config_defaults();
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_cfg.ssh_enabled = true;
    g_cfg.ssh_port = 2224;
    test_command = fixture_command;
    live_reset();
    ssh_stop();
    if (ssh_running()) return 1;
    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 2;
    if (!usb0_present()) return 3;

    g_work_kind = WORK_MODE;
    g_pending_kind = WORK_MODE;
    g_work_pending = false;
    g_apply_pid = 0;
    g_network_failed = false;
    g_boot_dumped = true;
    g_last_watchdog_ms = now_ms() - 60000;
    watchdog();
    /* request_work() dispatches immediately, so the proof is the selected
     * work kind and the forked worker, not a still-pending flag. */
    if (g_work_kind != WORK_SSH || g_apply_pid <= 0) return 4;
    if (g_pending_pair) return 5;
    if (waitpid(g_apply_pid, NULL, 0) != g_apply_pid) return 6;
    g_apply_pid = 0;
    g_work_kind = WORK_MODE;
    g_pending_kind = WORK_MODE;
    g_work_pending = false;
    unlink(WIFI1B_USB0_SYSFS);
    return 0;
}

int main(void)
{
    int rc;
    config_defaults();
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.ssh_enabled = true;
    g_cfg.ssh_port = 2224;
    test_command = fixture_command;
    live_reset();

    if ((rc = test_lifecycle()) != 0) return rc;
    if ((rc = test_home_only_key()) != 0) return rc;
    if ((rc = test_shutdown_and_wifi_failure()) != 0) return rc;
    if ((rc = test_hostile_helper_destinations()) != 0) return rc;
    if ((rc = test_hostile_rule_destination()) != 0) return rc;
    if ((rc = test_conflicting_key_destinations()) != 0) return rc;
    if ((rc = test_explicit_key_pair_import()) != 0) return rc;
    if ((rc = test_cleanup_idempotent_absent()) != 0) return rc;
    if ((rc = test_cleanup_no_owner_preserves()) != 0) return rc;
    if ((rc = test_cleanup_unknown_preserves()) != 0) return rc;
    if ((rc = test_cleanup_exact()) != 0) return rc;
    if ((rc = test_cleanup_fault_no_effect()) != 0) return rc;
    if ((rc = test_apply_exact()) != 0) return rc;
    if ((rc = test_unknown_regular_preserved()) != 0) return rc;
    if ((rc = test_known_port_update()) != 0) return rc;
    if ((rc = test_legacy_helper_adoption()) != 0) return rc;
    if ((rc = test_chain_parser_matrix()) != 0) return rc;
    if ((rc = test_cleanup_foreign_chain()) != 0) return rc;
    if ((rc = test_credential_parent_refusal()) != 0) return rc;
    if ((rc = test_incomplete_readbacks()) != 0) return rc;
    if ((rc = test_full_owner_records()) != 0) return rc;
    if ((rc = test_watchdog_usb_before_wifi_early_return()) != 0) return 300 + rc;
    if ((rc = test_ssh_worker_ignores_foreign_radio_owner()) != 0) return 320 + rc;

    /* Leave a known exact file set and no live state for the Python harness. */
    remove_usb0_files();
    live_reset();
    unlink(WIFI1B_USB0_SYSFS);
    if (!usb0_rule_write(2224)) return 200;
    test_command = NULL;
    return 0;
}
