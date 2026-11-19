/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline tests for receiver advertisement policy, listener ownership and the
 * bounded DNS-SD lookup (mock backend; no network).
 *
 * argv[1] mock libdns_sd.so with all symbols
 * argv[2] mock libdns_sd.so without DNSServiceResolve
 * argv[3] pin fixture root (contains a non-matching app/lib/libdns_sd.so)
 * argv[4] empty fixture root (pinned file absent)
 */

#define _POSIX_C_SOURCE 200809L

#include "cp_receiver_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    require(file != NULL, "open fixture");
    require(fputs(text, file) >= 0, "write fixture");
    require(fclose(file) == 0, "close fixture");
}

static void make_dir(const char *path)
{
    require(mkdir(path, 0700) == 0, "mkdir fixture");
}

static void make_path(const char *root, const char *suffix, char *out,
                      size_t cap)
{
    int written = snprintf(out, cap, "%s/%s", root, suffix);

    require(written > 0 && (size_t)written < cap, "fixture path");
}

static cp_receiver_observation good_observation(void)
{
    cp_receiver_observation observation;

    memset(&observation, 0, sizeof(observation));
    observation.state = CP_RECEIVER_CANDIDATE_VALID;
    observation.interface_index = 1u;
    observation.port = 7000u;
    strcpy(observation.service_name, "IONIQ5");
    strcpy(observation.device_id, "AA:BB:CC:DD:EE:01");
    strcpy(observation.source_version, "320.17.6");
    observation.have_pi = true;
    observation.pi_len = 16u;
    memset(observation.pi_sha256, 0x5a, sizeof(observation.pi_sha256));
    return observation;
}

static cp_receiver_policy_params good_params(void)
{
    cp_receiver_policy_params params;

    memset(&params, 0, sizeof(params));
    params.bt_mac = "AA:BB:CC:DD:EE:00";
    params.expected_device_id = "";
    params.expected_port = 0u;
    params.expected_source_version = "";
    params.expected_pi_sha256 = NULL;
    return params;
}

static void test_mac_derivation(void)
{
    char derived[18];

    require(cp_receiver_policy_derive_device_id("AA:BB:CC:DD:EE:FF", derived,
                                                sizeof(derived)) ==
                CP_RECEIVER_POLICY_OK,
            "derive FF boundary");
    /* Recorded stock behavior: a bare byte increment wraps, no carry. */
    require(strcmp(derived, "AA:BB:CC:DD:EE:00") == 0,
            "FF boundary wraps to 00");
    require(cp_receiver_policy_derive_device_id("AA:BB:CC:DD:EE:01", derived,
                                                sizeof(derived)) ==
                CP_RECEIVER_POLICY_OK,
            "derive second device id");
    require(strcmp(derived, "AA:BB:CC:DD:EE:02") == 0, "second increment");
    require(cp_receiver_policy_derive_device_id("00:00:00:00:00:00", derived,
                                                sizeof(derived)) ==
                CP_RECEIVER_POLICY_ERR_BT_MAC,
            "reject all-zero bt mac");
    require(cp_receiver_policy_derive_device_id("01:00:00:00:00:00", derived,
                                                sizeof(derived)) ==
                CP_RECEIVER_POLICY_ERR_BT_MAC,
            "reject multicast bt mac");
    require(cp_receiver_policy_derive_device_id("not-a-mac", derived,
                                                sizeof(derived)) ==
                CP_RECEIVER_POLICY_ERR_BT_MAC,
            "reject malformed bt mac");
}

static void test_policy(void)
{
    cp_receiver_observation observations[CP_OEM_RECEIVER_MAX_SERVICES];
    cp_receiver_policy_params params = good_params();
    cp_receiver_policy_result result;
    cp_receiver_policy_status status;
    uint8_t pi[CP_OEM_RECEIVER_SHA256_LEN];
    const char raw_pi[] = "S3cr3t-Pi-Value-Not-For-Output";
    uint8_t pin[CP_OEM_RECEIVER_SHA256_LEN];
    size_t i;

    /* Single valid candidate. */
    observations[0] = good_observation();
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_OK, "accept matching observation");
    require(strcmp(result.device_id, "AA:BB:CC:DD:EE:01") == 0,
            "return observed device id");
    require(result.port == 7000u, "return observed port");
    require(strcmp(result.source_version, "320.17.6") == 0,
            "return observed source version");
    require(result.pi_len == 16u, "carry pi length");

    /* Case-insensitive explicit match. */
    params.expected_device_id = "aa:bb:cc:dd:ee:01";
    params.expected_port = 7000u;
    params.expected_source_version = "320.17.6";
    observations[0] = good_observation();
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_OK, "accept explicit match");
    params = good_params();

    /* Invalid first candidate must not abort a later valid candidate. */
    observations[0] = good_observation();
    observations[0].port = 0u;
    observations[1] = good_observation();
    status = cp_receiver_policy_accept(observations, 2u, &params, &result);
    require(status == CP_RECEIVER_POLICY_OK,
            "one invalid port does not abort a valid candidate");

    observations[0] = good_observation();
    observations[0].state = CP_RECEIVER_CANDIDATE_TXT;
    observations[1] = good_observation();
    status = cp_receiver_policy_accept(observations, 2u, &params, &result);
    require(status == CP_RECEIVER_POLICY_OK,
            "one invalid TXT does not abort a valid candidate");

    /* Two valid distinct identities are ambiguous. */
    observations[0] = good_observation();
    observations[1] = good_observation();
    memset(observations[1].pi_sha256, 0x77,
           sizeof(observations[1].pi_sha256));
    status = cp_receiver_policy_accept(observations, 2u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_AMBIGUOUS,
            "two distinct passing candidates are ambiguous");

    observations[1] = good_observation();
    observations[1].port = 7001u;
    status = cp_receiver_policy_accept(observations, 2u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_AMBIGUOUS,
            "different ports are ambiguous");

    observations[1] = good_observation();
    strcpy(observations[1].device_id, "AA:BB:CC:DD:EE:02");
    status = cp_receiver_policy_accept(observations, 2u, &params, &result);
    /* The changed ID no longer matches bt_mac+1, so only one passes. */
    require(status == CP_RECEIVER_POLICY_OK,
            "one passing candidate after identity mismatch");

    /* Duplicate advertisement of the same identity is accepted once. */
    observations[0] = good_observation();
    observations[1] = good_observation();
    strcpy(observations[1].service_name, "IONIQ5-2");
    status = cp_receiver_policy_accept(observations, 2u, &params, &result);
    require(status == CP_RECEIVER_POLICY_OK,
            "identical identity twice is accepted once");

    /* Zero candidates. */
    status = cp_receiver_policy_accept(observations, 0u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_NO_SERVICE, "no candidates");

    /* All unresolved. */
    observations[0] = good_observation();
    observations[0].state = CP_RECEIVER_CANDIDATE_UNRESOLVED;
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_RESOLVE, "reject unresolved");

    /* Field mismatches. */
    observations[0] = good_observation();
    params.expected_port = 7001u;
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_PORT, "reject port mismatch");
    params = good_params();

    params.expected_device_id = "AA:BB:CC:DD:EE:02";
    observations[0] = good_observation();
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_DEVICE_ID,
            "reject device id mismatch");
    params = good_params();

    /* A matching pin is not enough if the address is unusable. */
    params.expected_device_id = "01:00:00:00:00:00";
    observations[0] = good_observation();
    strcpy(observations[0].device_id, "01:00:00:00:00:00");
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_DEVICE_ID,
            "reject multicast pinned device id");
    params = good_params();

    observations[0] = good_observation();
    strcpy(observations[0].device_id, "00:00:00:00:00:00");
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_DEVICE_ID,
            "reject all-zero observed device id");

    params.expected_source_version = "320.17.7";
    observations[0] = good_observation();
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_SOURCE_VERSION,
            "reject source version mismatch");
    params = good_params();

    observations[0] = good_observation();
    strcpy(observations[0].device_id, "AA:BB:CC:DD:EE:02");
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_DEVICE_ID,
            "reject advertisement not derived from bt mac");

    /* Missing pi is rejected. */
    observations[0] = good_observation();
    observations[0].have_pi = false;
    observations[0].pi_len = 0u;
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_PI, "reject missing pi");

    /* Pi pin matches by hash only. */
    require(cp_oem_receiver_sha256(raw_pi, strlen(raw_pi), pi) ==
                CP_OEM_RECEIVER_OK,
            "hash raw pi");
    observations[0] = good_observation();
    observations[0].pi_len = strlen(raw_pi);
    memcpy(observations[0].pi_sha256, pi, sizeof(pi));
    params.expected_pi_sha256 = pi;
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_OK, "accept matching pi pin");

    /* The raw pi value must not appear in any result bytes. */
    for (i = 0u; i < sizeof(result); i++) {
        require(memcmp((const unsigned char *)&result + i, raw_pi,
                       sizeof(raw_pi) - 1u) != 0,
                "raw pi absent from policy result");
    }

    memcpy(pin, pi, sizeof(pin));
    pin[0] ^= 0xffu;
    params.expected_pi_sha256 = pin;
    status = cp_receiver_policy_accept(observations, 1u, &params, &result);
    require(status == CP_RECEIVER_POLICY_ERR_PI, "reject pi pin mismatch");
    params = good_params();

    /* Pin parsing. */
    require(cp_receiver_policy_pin_parse(
                "0123456789abcdef0123456789abcdef"
                "0123456789abcdef0123456789abcdef",
                pin),
            "parse 64-hex pin");
    require(!cp_receiver_policy_pin_parse("00", pin), "reject short pin");
    require(!cp_receiver_policy_pin_parse(
                "zz23456789abcdef0123456789abcdef"
                "0123456789abcdef0123456789abcdef",
                pin),
            "reject non-hex pin");
}

static void test_json_escape(void)
{
    char out[64];

    require(cp_receiver_json_escape("plain", out, sizeof(out)), "escape plain");
    require(strcmp(out, "plain") == 0, "plain unchanged");
    require(cp_receiver_json_escape("a\"b\\c", out, sizeof(out)),
            "escape quote/backslash");
    require(strcmp(out, "a\\\"b\\\\c") == 0, "quote/backslash escaped");
    require(cp_receiver_json_escape("line\nfeed\t", out, sizeof(out)),
            "escape controls");
    require(strcmp(out, "line\\nfeed\\t") == 0, "controls escaped");
    require(cp_receiver_json_escape("\x01", out, sizeof(out)),
            "escape low control");
    require(strcmp(out, "\\u0001") == 0, "low control escaped");
    require(!cp_receiver_json_escape("abcdef", out, 4u),
            "report truncation");
    require(strcmp(out, "abc") == 0, "truncated output stays valid");
}

static void make_owner(const char *root, const char *pid)
{
    char path[256];
    char link_path[512];
    char comm[256];

    make_path(root, pid, path, sizeof(path));
    make_dir(path);
    snprintf(comm, sizeof(comm), "%s/%s/comm", root, pid);
    write_file(comm, "AppProjection\n");
    snprintf(comm, sizeof(comm), "%s/%s/fd", root, pid);
    make_dir(comm);
    snprintf(link_path, sizeof(link_path), "%s/3", comm);
    require(symlink("socket:[12345]", link_path) == 0, "fd symlink");
}

static void remove_fixture(const char *root, const char *name)
{
    char path[256];

    make_path(root, name, path, sizeof(path));
    require(unlink(path) == 0, "unlink fixture");
}

static void write_tcp_fixture(const char *root, const char *local)
{
    char path[256];
    char line[512];

    make_path(root, "net", path, sizeof(path));
    if (access(path, F_OK) != 0) {
        make_dir(path);
    }
    make_path(root, "net/tcp", path, sizeof(path));
    snprintf(line, sizeof(line),
             "  sl  local_address rem_address   st tx_queue rx_queue "
             "tr tm->when retrnsmt uid timeout inode\n"
             "   0: %s 00000000:0000 0A 00000000:00000000 00:00000000 "
             "00000000 0 0 12345 1 0 100 0 0 10 0\n",
             local);
    write_file(path, line);
}

static void write_tcp6_fixture(const char *root, const char *local)
{
    char path[256];
    char line[640];

    make_path(root, "net", path, sizeof(path));
    if (access(path, F_OK) != 0) {
        make_dir(path);
    }
    make_path(root, "net/tcp6", path, sizeof(path));
    snprintf(line, sizeof(line),
             "  sl  local_address rem_address   st tx_queue rx_queue "
             "tr tm->when retrnsmt uid timeout inode\n"
             "   0: %s 00000000000000000000000000000000:0000 0A "
             "00000000:00000000 00:00000000 00000000 0 0 12345 1 0 100 0 0\n",
             local);
    write_file(path, line);
}

static void test_listener(void)
{
    char root[] = "/tmp/opencode/cp-receiver-listener-XXXXXX";
    char empty_root[] = "/tmp/opencode/cp-receiver-listener-empty-XXXXXX";
    cp_receiver_listener_config config;
    cp_receiver_listener_result result;
    const char *addresses[1];

    require(mkdtemp(root) != NULL, "mkdtemp listener");
    require(mkdtemp(empty_root) != NULL, "mkdtemp empty listener");
    make_owner(root, "4242");

    memset(&config, 0, sizeof(config));
    config.proc_root = root;
    config.interface_name = "wlan0";
    config.owner_allowlist = "AppProjection";
    config.max_fd_scan = 64u;

    /* Wildcard IPv4 listener owned by AppProjection. */
    write_tcp_fixture(root, "00000000:1B58");
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_OK,
            "accept wildcard listener");
    require(result.found && result.wildcard && result.address_allowed,
            "wildcard flags");
    require(result.owner_pid == 4242, "inode owner pid");
    require(strcmp(result.owner_name, "AppProjection") == 0, "owner name");
    require(result.owner_allowed, "owner allowlisted");

    /* Specific receiver-interface address. */
    write_tcp_fixture(root, "0A0200C0:1B58");
    addresses[0] = "192.0.2.10";
    config.interface_addresses = addresses;
    config.interface_address_count = 1u;
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_OK,
            "accept interface-specific listener");
    require(!result.wildcard && result.address_allowed, "specific flags");

    /* A specific address that is not on the receiver interface. */
    write_tcp_fixture(root, "0101A8C0:1B58");
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_SCOPE,
            "reject foreign specific address");

    /* Loopback-only is a scope failure. */
    write_tcp_fixture(root, "0100007F:1B58");
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_SCOPE,
            "reject loopback-only listener");
    require(result.loopback_only, "loopback flag");

    /* IPv6 wildcard. */
    config.interface_addresses = NULL;
    config.interface_address_count = 0u;
    remove_fixture(root, "net/tcp");
    write_tcp6_fixture(root,
                       "00000000000000000000000000000000:1B58");
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_OK,
            "accept IPv6 wildcard listener");
    require(result.wildcard, "IPv6 wildcard flag");

    /* Allowlist mismatch is reported, not hidden. */
    write_tcp_fixture(root, "00000000:1B58");
    config.owner_allowlist = "SomeOtherProcess";
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_OWNER,
            "reject non-allowlisted owner");
    require(!result.owner_allowed, "owner not allowed");
    require(strcmp(result.owner_name, "AppProjection") == 0,
            "diagnostic still reports owner");

    /* Unknown inode owner. */
    config.owner_allowlist = "AppProjection";
    {
        char path[256];

        make_path(root, "net/tcp", path, sizeof(path));
        write_file(path,
                   "  sl  local_address rem_address   st\n"
                   "   0: 00000000:1B58 00000000:0000 0A 00000000:00000000 "
                   "00:00000000 00000000 0 0 99999 1 0 100 0 0\n");
    }
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_OWNER,
            "reject unknown owner");

    /* Absent listener. */
    require(cp_receiver_listener_check(7001u, &config, &result) ==
                CP_RECEIVER_LISTENER_ABSENT,
            "report absent listener");

    /* Unreadable proc root fails closed. */
    config.proc_root = empty_root;
    require(cp_receiver_listener_check(7000u, &config, &result) ==
                CP_RECEIVER_LISTENER_IO,
            "missing proc fails closed");
}

static void reset_mock_env(void)
{
    (void)unsetenv("CP_MOCK_SERVICES");
    (void)unsetenv("CP_MOCK_NAME");
    (void)unsetenv("CP_MOCK_PORT");
    (void)unsetenv("CP_MOCK_PI");
    (void)unsetenv("CP_MOCK_DEVICEID");
    (void)unsetenv("CP_MOCK_DUPLICATE_ADD");
    (void)unsetenv("CP_MOCK_INVALID_FIRST");
    (void)unsetenv("CP_MOCK_NONE");
    (void)unsetenv("CP_MOCK_NO_RESOLVE_REPLY");
}

static void lookup_config(cp_receiver_lookup_config *config,
                          const char *backend_library)
{
    memset(config, 0, sizeof(*config));
    strcpy(config->interface_name, "lo");
    config->interface_index = 1u;
    config->timeout_ms = 2000u;
    config->settle_ms = 100u;
    config->max_services = CP_OEM_RECEIVER_MAX_SERVICES;
    config->backend_library = backend_library;
    config->allow_unpinned_backend = true;
}

static int stop_after_two(void *context)
{
    unsigned *counter = context;

    (*counter)++;
    return *counter >= 2u;
}

static void test_lookup(const char *mock, const char *missing_symbol,
                        const char *pin_root, const char *empty_root)
{
    cp_receiver_lookup_config config;
    cp_receiver_lookup_result result;
    cp_receiver_lookup_status status;
    unsigned stop_counter = 0u;

    reset_mock_env();
    (void)setenv("CP_MOCK_PORT", "7000", 1);
    (void)setenv("CP_MOCK_NAME", "MockReceiver", 1);
    lookup_config(&config, mock);
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_OK, "mock success status");
    require(result.service_count == 1u, "mock distinct instance count");
    require(result.resolved_count == 1u, "mock resolved count");
    require(result.rejected_count == 0u, "mock rejected count");
    require(result.services[0].port == 7000u,
            "network-order 0x1B58 becomes 7000");
    require(strcmp(result.services[0].device_id, "AA:BB:CC:DD:EE:01") == 0,
            "mock device id");
    require(strcmp(result.services[0].source_version, "320.17.6") == 0,
            "mock source version");
    require(result.services[0].have_pi &&
                result.services[0].pi_len == 32u,
            "mock pi length only");
    require(strcmp(result.services[0].service_name, "MockReceiver") == 0,
            "mock service name");

    /* Duplicate Add for the same identity is not ambiguity. */
    reset_mock_env();
    (void)setenv("CP_MOCK_PORT", "7000", 1);
    (void)setenv("CP_MOCK_NAME", "MockReceiver", 1);
    (void)setenv("CP_MOCK_DUPLICATE_ADD", "1", 1);
    lookup_config(&config, mock);
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_OK, "duplicate Add status");
    require(result.service_count == 1u, "duplicate Add is one instance");
    require(!result.ambiguous, "duplicate Add is not ambiguous");

    /* Two distinct services: lookup reports both, policy refuses. */
    reset_mock_env();
    (void)setenv("CP_MOCK_SERVICES", "2", 1);
    (void)setenv("CP_MOCK_NAME", "MockReceiver", 1);
    lookup_config(&config, mock);
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_OK, "two-service lookup status");
    require(result.service_count == 2u, "two distinct instances");
    require(result.resolved_count == 2u, "two resolved candidates");
    {
        cp_receiver_policy_params params = good_params();
        cp_receiver_policy_result accepted;
        cp_receiver_policy_status policy;

        policy = cp_receiver_policy_accept(result.services, 2u, &params,
                                           &accepted);
        require(policy == CP_RECEIVER_POLICY_ERR_AMBIGUOUS,
                "two valid distinct services are ambiguous");
    }

    /* One invalid TXT candidate and one valid candidate. */
    reset_mock_env();
    (void)setenv("CP_MOCK_SERVICES", "2", 1);
    (void)setenv("CP_MOCK_NAME", "MockReceiver", 1);
    (void)setenv("CP_MOCK_INVALID_FIRST", "1", 1);
    lookup_config(&config, mock);
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_OK, "invalid-first lookup status");
    require(result.resolved_count == 1u &&
                result.rejected_count == 1u,
            "one rejected, one resolved");
    {
        cp_receiver_policy_params params = good_params();
        cp_receiver_policy_result accepted;
        cp_receiver_policy_status policy;

        policy = cp_receiver_policy_accept(result.services, 2u, &params,
                                           &accepted);
        require(policy == CP_RECEIVER_POLICY_OK,
                "one invalid TXT plus one valid is accepted");
    }

    /* A single rejected candidate is still an OK lookup so policy can name
     * the reason; it must not collapse into a generic timeout. */
    reset_mock_env();
    (void)setenv("CP_MOCK_SERVICES", "1", 1);
    (void)setenv("CP_MOCK_NAME", "MockReceiver", 1);
    (void)setenv("CP_MOCK_INVALID_FIRST", "1", 1);
    lookup_config(&config, mock);
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_OK, "invalid-only lookup status");
    require(result.resolved_count == 0u && result.rejected_count == 1u,
            "invalid-only rejected count");
    require(result.services[0].state == CP_RECEIVER_CANDIDATE_TXT,
            "invalid-only candidate state");
    {
        cp_receiver_policy_params params = good_params();
        cp_receiver_policy_result accepted;
        cp_receiver_policy_status policy;

        policy = cp_receiver_policy_accept(result.services, 1u, &params,
                                           &accepted);
        require(policy == CP_RECEIVER_POLICY_ERR_TXT,
                "invalid-only policy reports txt");
    }

    /* No service. */
    reset_mock_env();
    (void)setenv("CP_MOCK_NONE", "1", 1);
    lookup_config(&config, mock);
    config.timeout_ms = 300u;
    config.settle_ms = 50u;
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_NO_SERVICE, "no-service status");
    require(result.service_count == 0u, "no-service count zero");
    require(result.services[0].state == CP_RECEIVER_CANDIDATE_UNRESOLVED,
            "no-service zeroed observation");

    /* Instances appear but never resolve. */
    reset_mock_env();
    (void)setenv("CP_MOCK_NO_RESOLVE_REPLY", "1", 1);
    lookup_config(&config, mock);
    config.timeout_ms = 300u;
    config.settle_ms = 50u;
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_TIMEOUT, "timeout distinct from no-service");
    require(result.service_count == 1u, "timeout keeps counter");

    /* Stop request. */
    reset_mock_env();
    (void)setenv("CP_MOCK_NONE", "1", 1);
    lookup_config(&config, mock);
    config.timeout_ms = 5000u;
    stop_counter = 0u;
    status = cp_receiver_lookup_live(&config, stop_after_two, &stop_counter,
                                     &result);
    require(status == CP_RECEIVER_LOOKUP_STOPPED, "stop mid-lookup");
    require(result.service_count == 0u, "stopped keeps zero observation");

    /* Missing symbol. */
    reset_mock_env();
    lookup_config(&config, missing_symbol);
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_NO_BACKEND, "missing symbol");

    /* Pinned backend mismatch and absence. */
    lookup_config(&config, NULL);
    config.allow_unpinned_backend = false;
    config.backend_root = pin_root;
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_BACKEND_PIN, "pin mismatch");
    config.backend_root = empty_root;
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_BACKEND_PIN, "missing pinned file");

    /* Argument rejection. */
    lookup_config(&config, mock);
    config.interface_index = 0u;
    status = cp_receiver_lookup_live(&config, NULL, NULL, &result);
    require(status == CP_RECEIVER_LOOKUP_ARGUMENT, "zero interface index");
}

int main(int argc, char **argv)
{
    cp_receiver_observation observations[1];
    cp_receiver_policy_params params = good_params();
    cp_receiver_policy_result result;

    require(argc == 5, "usage: test mock missing-symbol pin-root empty-root");
    test_mac_derivation();
    test_policy();
    test_json_escape();
    test_listener();

    /* The MAC boundary test above records the stock byte-increment wrap. */
    observations[0] = good_observation();
    strcpy(observations[0].device_id, "AA:BB:CC:DD:EE:00");
    params.bt_mac = "AA:BB:CC:DD:EE:FF";
    require(cp_receiver_policy_accept(observations, 1u, &params, &result) ==
                CP_RECEIVER_POLICY_OK,
            "FF boundary device id accepted");
    params = good_params();

    test_lookup(argv[1], argv[2], argv[3], argv[4]);
    puts("PASS: receiver policy, listener and lookup tests");
    return 0;
}
