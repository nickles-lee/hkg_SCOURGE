/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Unit tests for the passive OEM receiver probe.  No network, OEM execution,
 * keychain access or real fixture hardware is used.
 */

#define _POSIX_C_SOURCE 200809L

#include "cp_oem_receiver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, message)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "not ok - %s\n", (message));                         \
            failures++;                                                          \
            return;                                                              \
        }                                                                        \
        printf("ok - %s\n", (message));                                          \
    } while (0)

typedef struct {
    cp_oem_receiver_dns_event_fn event_fn;
    void *event_context;
    uint32_t interface_index;
    unsigned starts;
    unsigned stops;
    cp_oem_receiver_status start_status;
} mock_dns;

static cp_oem_receiver_status mock_start(void *context, const char *service_type,
                                         const char *domain, uint32_t interface_index,
                                         cp_oem_receiver_dns_event_fn event_fn,
                                         void *event_context)
{
    mock_dns *mock = (mock_dns *)context;

    (void)service_type;
    (void)domain;
    mock->starts++;
    mock->interface_index = interface_index;
    mock->event_fn = event_fn;
    mock->event_context = event_context;
    return mock->start_status;
}

static void mock_stop(void *context)
{
    mock_dns *mock = (mock_dns *)context;

    mock->stops++;
}

static cp_oem_receiver_dns_ops mock_ops(mock_dns *mock)
{
    cp_oem_receiver_dns_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = mock;
    ops.start = mock_start;
    ops.stop = mock_stop;
    return ops;
}

static void mock_emit(mock_dns *mock, const cp_oem_receiver_dns_event *event)
{
    if (mock->event_fn != NULL) {
        mock->event_fn(mock->event_context, event);
    }
}

static void event_base(cp_oem_receiver_dns_event *event, cp_oem_receiver_dns_event_type type,
                       const char *name, uint32_t ifindex, uint64_t now_ms)
{
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->observed_ms = now_ms;
    event->interface_index = ifindex;
    event->service_name = name;
    event->service_name_len = name != NULL ? strlen(name) : 0u;
    event->regtype = CP_OEM_RECEIVER_SERVICE_TYPE;
    event->regtype_len = strlen(CP_OEM_RECEIVER_SERVICE_TYPE);
    event->domain = CP_OEM_RECEIVER_SERVICE_DOMAIN;
    event->domain_len = strlen(CP_OEM_RECEIVER_SERVICE_DOMAIN);
}

static size_t txt_add(uint8_t *buf, size_t cap, size_t offset, const char *text)
{
    size_t len = strlen(text);

    if (len > 255u || offset + 1u + len > cap) {
        return 0u;
    }
    buf[offset++] = (uint8_t)len;
    memcpy(buf + offset, text, len);
    return offset + len;
}

static size_t txt_add_raw(uint8_t *buf, size_t cap, size_t offset, const uint8_t *data,
                          size_t len)
{
    if (len > 255u || offset + 1u + len > cap) {
        return 0u;
    }
    buf[offset++] = (uint8_t)len;
    memcpy(buf + offset, data, len);
    return offset + len;
}

static size_t make_txt(uint8_t *buf, size_t cap, const char *device_id, const char *pi,
                       const char *source_version)
{
    size_t offset = 0u;

    offset = txt_add(buf, cap, offset, device_id);
    offset = txt_add(buf, cap, offset, pi);
    offset = txt_add(buf, cap, offset, source_version);
    return offset;
}

static void setup_probe(cp_oem_receiver *probe, mock_dns *mock,
                        cp_oem_receiver_config *config, uint32_t ifindex,
                        uint32_t max_services, uint64_t settle_ms,
                        uint64_t browse_timeout_ms, uint64_t resolve_timeout_ms,
                        uint64_t stale_after_ms)
{
    cp_oem_receiver_dns_ops ops;

    memset(mock, 0, sizeof(*mock));
    mock->start_status = CP_OEM_RECEIVER_OK;
    cp_oem_receiver_config_default(config);
    config->interface_index = ifindex;
    config->max_services = max_services;
    config->settle_ms = settle_ms;
    config->browse_timeout_ms = browse_timeout_ms;
    config->resolve_timeout_ms = resolve_timeout_ms;
    config->stale_after_ms = stale_after_ms;
    ops = mock_ops(mock);
    if (cp_oem_receiver_init(probe, config, &ops) != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "probe init failed\n");
        exit(2);
    }
    if (cp_oem_receiver_start(probe, 0u) != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "probe start failed\n");
        exit(2);
    }
}

static void add_service(cp_oem_receiver *probe, mock_dns *mock, const char *name,
                        uint32_t ifindex, uint64_t now_ms)
{
    cp_oem_receiver_dns_event event;

    event_base(&event, CP_OEM_RECEIVER_DNS_ADD, name, ifindex, now_ms);
    mock_emit(mock, &event);
    (void)probe;
}

static void resolve_service(cp_oem_receiver *probe, mock_dns *mock, const char *name,
                            uint32_t ifindex, uint16_t port, const uint8_t *txt,
                            size_t txt_len, uint64_t now_ms)
{
    cp_oem_receiver_dns_event event;

    (void)probe;
    event_base(&event, CP_OEM_RECEIVER_DNS_RESOLVE, name, ifindex, now_ms);
    event.port = port;
    event.txt = txt;
    event.txt_len = txt_len;
    mock_emit(mock, &event);
}

static void remove_service(cp_oem_receiver *probe, mock_dns *mock, const char *name,
                           uint32_t ifindex, uint64_t now_ms)
{
    cp_oem_receiver_dns_event event;

    (void)probe;
    event_base(&event, CP_OEM_RECEIVER_DNS_REMOVE, name, ifindex, now_ms);
    mock_emit(mock, &event);
}

static void set_ready_scope(cp_oem_receiver *probe, uint32_t ifindex, uint64_t now_ms)
{
    cp_oem_receiver_scope scope;

    if (cp_oem_receiver_scope_init(&scope, "wlan0", ifindex, "fe80::1%wlan0", true) !=
        CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "scope init failed\n");
        exit(2);
    }
    (void)cp_oem_receiver_set_scope(probe, &scope, now_ms);
}

static void set_attestation(cp_oem_receiver *probe, uint16_t port, const char *pi,
                            uint64_t now_ms)
{
    cp_oem_receiver_attestation attestation;
    cp_oem_receiver_status status;

    memset(&attestation, 0, sizeof(attestation));
    attestation.receiver_observed = true;
    attestation.receiver_pid = 1234u;
    attestation.listener_port = port;
    attestation.identity_fingerprint_present = true;
    status = cp_oem_receiver_sha256(pi, strlen(pi), attestation.identity_fingerprint);
    if (status != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "sha256 failed\n");
        exit(2);
    }
    (void)cp_oem_receiver_set_attestation(probe, &attestation, now_ms);
}

static void test_txt_valid(void)
{
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=0123456789abcdef", "srcvers=320.17.6");
    cp_oem_receiver_txt parsed;

    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) == CP_OEM_RECEIVER_OK,
          "valid TXT parses");
    CHECK(parsed.have_device_id && parsed.device_id_len == 17u &&
              strcmp(parsed.device_id, "AA:BB:CC:DD:EE:FF") == 0,
          "deviceid captured");
    CHECK(parsed.have_pi && strcmp(parsed.pi, "0123456789abcdef") == 0, "pi captured");
    CHECK(parsed.have_source_version &&
              strcmp(parsed.source_version, CP_OEM_RECEIVER_SOURCE_VERSION) == 0,
          "srcvers captured");
    CHECK(parsed.pi_sha256[0] != 0u || parsed.pi_sha256[1] != 0u,
          "pi fingerprint computed");
}

static void test_txt_missing(void)
{
    uint8_t txt[128];
    size_t len = txt_add(txt, sizeof(txt), 0u, "deviceid=AA:BB:CC:DD:EE:FF");
    cp_oem_receiver_txt parsed;

    len = txt_add(txt, sizeof(txt), len, "srcvers=320.17.6");
    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_MISSING,
          "missing pi rejected");
}

static void test_txt_duplicate(void)
{
    uint8_t txt[256];
    size_t len = txt_add(txt, sizeof(txt), 0u, "deviceid=AA:BB:CC:DD:EE:FF");
    cp_oem_receiver_txt parsed;

    len = txt_add(txt, sizeof(txt), len, "deviceid=AA:BB:CC:DD:EE:FF");
    len = txt_add(txt, sizeof(txt), len, "pi=0123456789abcdef");
    len = txt_add(txt, sizeof(txt), len, "srcvers=320.17.6");
    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_DUPLICATE,
          "duplicate key rejected");
}

static void test_txt_flag_only_unknown(void)
{
    uint8_t txt[256];
    size_t len = txt_add(txt, sizeof(txt), 0u, "deviceid=AA:BB:CC:DD:EE:FF");
    cp_oem_receiver_txt parsed;

    len = txt_add(txt, sizeof(txt), len, "pi=0123456789abcdef");
    len = txt_add(txt, sizeof(txt), len, "srcvers=320.17.6");
    len = txt_add(txt, sizeof(txt), len, "unknown-flag");
    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) == CP_OEM_RECEIVER_OK,
          "unknown flag-only TXT key accepted");
    CHECK(parsed.unknown_count == 1u, "unknown flag-only key counted");
}

static void test_txt_required_without_value(void)
{
    uint8_t txt[256];
    size_t len = txt_add(txt, sizeof(txt), 0u, "deviceid");
    cp_oem_receiver_txt parsed;

    len = txt_add(txt, sizeof(txt), len, "pi=0123456789abcdef");
    len = txt_add(txt, sizeof(txt), len, "srcvers=320.17.6");
    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_VALUE,
          "required key without value rejected");
}

static void test_txt_malformed(void)
{
    uint8_t txt[8] = { 7u, (uint8_t)'p', (uint8_t)'i', (uint8_t)'=', 0u, 0u, 0u, 0u };
    cp_oem_receiver_txt parsed;

    CHECK(cp_oem_receiver_parse_txt(txt, sizeof(txt), &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_MALFORMED,
          "length overrun rejected");
}

static void test_txt_nul(void)
{
    static const uint8_t value[] = { 'p', 'i', '=', 'a', 0u, 'b' };
    uint8_t txt[16];
    size_t len = txt_add_raw(txt, sizeof(txt), 0u, value, sizeof(value));
    cp_oem_receiver_txt parsed;

    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_MALFORMED,
          "embedded NUL rejected");
}

static void test_txt_invalid_utf8(void)
{
    static const uint8_t value[] = { 'p', 'i', '=', 0xc3u, 0x28u };
    uint8_t txt[64];
    size_t len = txt_add_raw(txt, sizeof(txt), 0u, value, sizeof(value));
    cp_oem_receiver_txt parsed;

    len = txt_add(txt, sizeof(txt), len, "deviceid=AA:BB:CC:DD:EE:FF");
    len = txt_add(txt, sizeof(txt), len, "srcvers=320.17.6");
    CHECK(cp_oem_receiver_parse_txt(txt, len, &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_UTF8,
          "invalid UTF-8 rejected");
}

static void test_txt_too_long(void)
{
    uint8_t byte = 0u;
    cp_oem_receiver_txt parsed;

    CHECK(cp_oem_receiver_parse_txt(&byte, CP_OEM_RECEIVER_MAX_TXT + 1u, &parsed) ==
              CP_OEM_RECEIVER_ERR_BOUNDS,
          "oversized TXT rejected");
    CHECK(cp_oem_receiver_parse_txt(NULL, 0u, &parsed) ==
              CP_OEM_RECEIVER_ERR_TXT_MISSING,
          "empty TXT is missing fields");
}

static void test_srv_validation(void)
{
    CHECK(cp_oem_receiver_validate_srv(3u, 3u, 0u, "CarPlay", 7u) ==
              CP_OEM_RECEIVER_ERR_SRV_PORT,
          "SRV port zero rejected");
    CHECK(cp_oem_receiver_validate_srv(4u, 3u, 7000u, "CarPlay", 7u) ==
              CP_OEM_RECEIVER_ERR_SRV_INTERFACE,
          "SRV wrong interface rejected");
    CHECK(cp_oem_receiver_validate_srv(3u, 3u, 7000u, NULL, 0u) ==
              CP_OEM_RECEIVER_ERR_SRV_NAME,
          "SRV missing name rejected");
    CHECK(cp_oem_receiver_validate_srv(3u, 3u, 7000u, "CarPlay", 7u) ==
              CP_OEM_RECEIVER_OK,
          "valid SRV accepted");
}

static void test_scope(void)
{
    cp_oem_receiver_scope scope;
    char text[80];

    CHECK(cp_oem_receiver_link_local_usable("fe80::1"), "bare link-local accepted");
    CHECK(cp_oem_receiver_link_local_usable("FE80::1%wlan0"), "zoned link-local accepted");
    CHECK(!cp_oem_receiver_link_local_usable("2001:db8::1"), "global address rejected");
    CHECK(!cp_oem_receiver_link_local_usable("fe80::"), "zero interface id rejected");
    CHECK(!cp_oem_receiver_link_local_usable("fe80::1::2"), "double compression rejected");
    CHECK(cp_oem_receiver_link_local_usable("fe80::1%3"), "numeric zone accepted");

    CHECK(cp_oem_receiver_scope_init(&scope, "wlan0", 3u, "fe80::1%wlan0", true) ==
              CP_OEM_RECEIVER_OK,
          "scope with matching zone accepted");
    CHECK(scope.has_link_local && scope.zone_matches && cp_oem_receiver_scope_ready(&scope),
          "ready scope reports ready");
    CHECK(cp_oem_receiver_scope_format(&scope, true, text, sizeof(text)) ==
              CP_OEM_RECEIVER_OK &&
              strcmp(text, "fe80::1%wlan0") == 0,
          "scope formats with zone");
    CHECK(cp_oem_receiver_scope_format(&scope, false, text, sizeof(text)) ==
              CP_OEM_RECEIVER_OK &&
              strcmp(text, "fe80::1") == 0,
          "scope formats without zone");
    CHECK(cp_oem_receiver_scope_format(&scope, true, text, 4u) ==
              CP_OEM_RECEIVER_ERR_BOUNDS,
          "short scope buffer rejected");
    CHECK(cp_oem_receiver_scope_init(&scope, "wlan0", 3u, "fe80::1", false) ==
              CP_OEM_RECEIVER_OK &&
              !cp_oem_receiver_scope_ready(&scope),
          "scope without confirmed local address is not ready");
    CHECK(cp_oem_receiver_scope_init(&scope, "wlan0", 3u, NULL, true) ==
              CP_OEM_RECEIVER_OK &&
              !cp_oem_receiver_scope_ready(&scope),
          "scope without link-local is not ready");
    CHECK(cp_oem_receiver_scope_init(&scope, "wlan0", 3u, "fe80::1%eth0", true) ==
              CP_OEM_RECEIVER_ERR_SCOPE,
          "mismatched zone rejected");
    CHECK(cp_oem_receiver_scope_init(&scope, "wlan0", 3u, "fe80::1%4", true) ==
              CP_OEM_RECEIVER_ERR_SCOPE,
          "numeric zone mismatch rejected");
    CHECK(cp_oem_receiver_scope_init(&scope, "wlan0", 3u, "2001:db8::1", true) ==
              CP_OEM_RECEIVER_ERR_SCOPE,
          "global address rejected by scope");
}

static void test_observation_unverified(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-one", "srcvers=320.17.6");
    uint32_t changed = 0u;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    CHECK(cp_oem_receiver_poll(&probe, 130u, &changed) == CP_OEM_RECEIVER_OK,
          "poll succeeds");
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.state == CP_OEM_RECEIVER_STATE_OBSERVED, "observed state");
    CHECK(snapshot.evidence == CP_OEM_RECEIVER_EVIDENCE_MDNS_UNVERIFIED,
          "peer TXT remains unverified");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_OBSERVED_UNVERIFIED,
          "peer TXT is not ready");
    CHECK(!snapshot.local_trusted, "no local attestation is not trusted");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_IDENTITY_ATTESTATION_MISSING) !=
              0u,
          "missing identity attestation blocker");
    CHECK(snapshot.interface_ready, "ready scope is reported separately");
    CHECK(snapshot.port == 7000u, "port captured");    CHECK(strcmp(snapshot.device_id, "AA:BB:CC:DD:EE:FF") == 0, "deviceid captured");
    CHECK(strcmp(snapshot.source_version, "320.17.6") == 0, "source version captured");
    CHECK(snapshot.projection == CP_OEM_RECEIVER_PROJECTION_UNAVAILABLE,
          "projection remains unavailable");
    CHECK(snapshot.service_count == 1u, "one service tracked");
}

static void test_observation_without_scope(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-no-scope", "srcvers=320.17.6");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot without scope");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_OBSERVED_UNVERIFIED,
          "observation without scope remains unverified");
    CHECK(!snapshot.interface_ready, "interface readiness is separate");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_INTERFACE_NOT_READY) != 0u,
          "interface-not-ready blocker present");
    CHECK(!snapshot.local_trusted, "no scope cannot be trusted");
}

static void test_local_attestation_ready(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot before;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-two", "srcvers=320.17.6");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &before) == CP_OEM_RECEIVER_OK,
          "snapshot before attestation");
    set_attestation(&probe, 7000u, "identity-two", 140u);
    (void)cp_oem_receiver_poll(&probe, 150u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.readiness_generation > before.readiness_generation,
          "readiness generation advanced after attestation");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_LOCAL_MATCHED,
          "matching local attestation is ready");
    CHECK(snapshot.evidence == CP_OEM_RECEIVER_EVIDENCE_LOCAL_ATTESTED,
          "local attestation evidence");
    CHECK(snapshot.local_trusted && snapshot.local_port_matches &&
              snapshot.local_identity_matches,
          "local attestation flags");
    CHECK(snapshot.interface_ready && snapshot.link_local_usable,
          "interface scope is ready");
    cp_oem_receiver_stop(&probe);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot after stop");
    CHECK(snapshot.state == CP_OEM_RECEIVER_STATE_IDLE && !snapshot.local_trusted &&
              snapshot.port == 0u,
          "stop clears trusted observation state");
}

static void test_local_attestation_mismatch(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-three", "srcvers=320.17.6");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    set_attestation(&probe, 7001u, "identity-other", 30u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_OBSERVED_UNVERIFIED,
          "mismatch remains unverified");
    CHECK(!snapshot.local_trusted, "mismatch not trusted");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_LOCAL_PORT_MISMATCH) != 0u,
          "port mismatch blocker");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_LOCAL_IDENTITY_MISMATCH) != 0u,
          "identity mismatch blocker");
}

static void test_interface_change(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-four", "srcvers=320.17.6");
    uint32_t generation;
    uint32_t ready_generation;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    generation = snapshot.observation_generation;
    ready_generation = snapshot.readiness_generation;
    CHECK(snapshot.service_count == 1u, "service before interface change");
    set_ready_scope(&probe, 4u, 140u);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot after interface change");
    CHECK(snapshot.service_count == 0u, "services invalidated on interface change");
    CHECK(snapshot.observation_generation > generation,
          "observation generation advanced on interface change");
    CHECK(snapshot.readiness_generation > ready_generation,
          "readiness generation advanced on interface change");
    CHECK(mock.starts >= 2u && mock.stops >= 1u,
          "backend restarted on interface change");
}

static void test_removal(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-five", "srcvers=320.17.6");
    uint32_t generation;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    (void)cp_oem_receiver_get_snapshot(&probe, &snapshot);
    generation = snapshot.observation_generation;
    remove_service(&probe, &mock, "CarPlay", 3u, 140u);
    (void)cp_oem_receiver_poll(&probe, 150u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.service_count == 0u && snapshot.port == 0u,
          "observation cleared on removal");
    CHECK(snapshot.observation_generation > generation,
          "generation advanced on removal");
    CHECK(snapshot.readiness != CP_OEM_RECEIVER_READINESS_LOCAL_MATCHED,
          "removal cannot stay ready");
}

static void test_changed_port(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-six", "srcvers=320.17.6");
    uint32_t generation;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    (void)cp_oem_receiver_get_snapshot(&probe, &snapshot);
    generation = snapshot.observation_generation;
    resolve_service(&probe, &mock, "CarPlay", 3u, 7001u, txt, len, 140u);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot after changed port");
    CHECK(snapshot.port == 7001u, "changed port applied");
    CHECK(snapshot.observation_generation > generation,
          "generation advanced on changed port");
    CHECK(!snapshot.stable, "changed port resets stability");
}

static void test_timeout(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    (void)cp_oem_receiver_poll(&probe, 1001u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.state == CP_OEM_RECEIVER_STATE_TIMEOUT, "browse timeout state");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_TIMEOUT,
          "browse timeout readiness");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_TIMEOUT) != 0u,
          "timeout blocker");
}

static void test_resolve_timeout(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    (void)cp_oem_receiver_poll(&probe, 600u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.port == 0u, "unresolved service has no port");
    CHECK(snapshot.service_count == 0u, "unresolved service released after timeout");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_RESOLVE_TIMEOUT) != 0u,
          "resolve timeout blocker");
}

static void test_too_many_services(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;

    setup_probe(&probe, &mock, &config, 3u, 1u, 100u, 1000u, 500u, 5000u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    add_service(&probe, &mock, "Second", 3u, 11u);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.service_count == 1u, "service count bounded");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_TOO_MANY_SERVICES) != 0u,
          "too many services blocker");
}

static void test_ambiguous(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt_one[256];
    uint8_t txt_two[256];
    size_t len_one = make_txt(txt_one, sizeof(txt_one), "deviceid=AA:BB:CC:DD:EE:01",
                              "pi=identity-a", "srcvers=320.17.6");
    size_t len_two = make_txt(txt_two, sizeof(txt_two), "deviceid=AA:BB:CC:DD:EE:02",
                              "pi=identity-b", "srcvers=320.17.6");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay-A", 3u, 10u);
    add_service(&probe, &mock, "CarPlay-B", 3u, 11u);
    resolve_service(&probe, &mock, "CarPlay-A", 3u, 7000u, txt_one, len_one, 20u);
    resolve_service(&probe, &mock, "CarPlay-B", 3u, 7000u, txt_two, len_two, 21u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.ambiguous && snapshot.service_count == 2u,
          "two valid advertisements are ambiguous");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_AMBIGUOUS,
          "ambiguous readiness");
    CHECK(snapshot.port == 0u, "ambiguous selection has no port");

    set_attestation(&probe, 7000u, "identity-a", 140u);
    (void)cp_oem_receiver_poll(&probe, 150u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot after disambiguation");
    CHECK(!snapshot.ambiguous && snapshot.readiness ==
              CP_OEM_RECEIVER_READINESS_LOCAL_MATCHED,
          "local attestation disambiguates");
}

static void test_stale(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-seven", "srcvers=320.17.6");
    uint32_t generation;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    (void)cp_oem_receiver_get_snapshot(&probe, &snapshot);
    generation = snapshot.observation_generation;
    (void)cp_oem_receiver_poll(&probe, 5021u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.port == 0u, "stale observation cleared");
    CHECK(snapshot.readiness == CP_OEM_RECEIVER_READINESS_STALE,
          "stale readiness");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_STALE) != 0u,
          "stale blocker");
    CHECK(snapshot.observation_generation > generation,
          "stale generation advanced");
}

static void test_source_version_mismatch(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-eight", "srcvers=999.0");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.port == 0u, "unexpected source version is not selected");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_UNEXPECTED_SOURCE_VERSION) != 0u,
          "unexpected source version blocker");
}

static void test_remote_interface_ignored(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    cp_oem_receiver_dns_event event;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    event_base(&event, CP_OEM_RECEIVER_DNS_ADD, "CarPlay", 4u, 10u);
    mock_emit(&mock, &event);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot after remote interface event");
    CHECK(snapshot.service_count == 0u && snapshot.port == 0u,
          "remote interface advertisement ignored");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_SRV_INVALID) != 0u ||
              (snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION) != 0u,
          "remote advertisement cannot become trusted");
}

static void test_port_zero_rejected(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    cp_oem_receiver_dns_event event;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-port-zero", "srcvers=320.17.6");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    event_base(&event, CP_OEM_RECEIVER_DNS_RESOLVE, "CarPlay", 3u, 20u);
    event.port = 0u;
    event.txt = txt;
    event.txt_len = len;
    mock_emit(&mock, &event);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot after zero port");
    CHECK(snapshot.port == 0u, "port zero is not selected");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_SRV_INVALID) != 0u,
          "port zero blocker");
}

static void test_unsolicited_resolve(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=identity-nine", "srcvers=320.17.6");

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.port == 0u && snapshot.service_count == 0u,
          "resolve without add is ignored");
}

static void test_dns_error(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    cp_oem_receiver_dns_event event;

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    memset(&event, 0, sizeof(event));
    event.type = CP_OEM_RECEIVER_DNS_ERROR;
    event.observed_ms = 10u;
    event.error = -1;
    mock_emit(&mock, &event);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(snapshot.state == CP_OEM_RECEIVER_STATE_ERROR, "DNS error state");
    CHECK((snapshot.blockers & CP_OEM_RECEIVER_BLOCKER_DNS_ERROR) != 0u,
          "DNS error blocker");
}

static void test_start_failure(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_dns_ops ops;
    cp_oem_receiver_snapshot snapshot;

    memset(&mock, 0, sizeof(mock));
    mock.start_status = CP_OEM_RECEIVER_ERR_DNS;
    cp_oem_receiver_config_default(&config);
    config.interface_index = 3u;
    ops = mock_ops(&mock);
    CHECK(cp_oem_receiver_init(&probe, &config, &ops) == CP_OEM_RECEIVER_OK,
          "init with failing backend");
    CHECK(cp_oem_receiver_start(&probe, 0u) == CP_OEM_RECEIVER_ERR_DNS,
          "backend start failure propagated");
    (void)cp_oem_receiver_get_snapshot(&probe, &snapshot);
    CHECK(snapshot.state == CP_OEM_RECEIVER_STATE_ERROR,
          "backend start failure state");
}

static void test_safe_summary(void)
{
    cp_oem_receiver probe;
    mock_dns mock;
    cp_oem_receiver_config config;
    cp_oem_receiver_snapshot snapshot;
    uint8_t txt[256];
    size_t len = make_txt(txt, sizeof(txt), "deviceid=AA:BB:CC:DD:EE:FF",
                          "pi=super-secret-identity", "srcvers=320.17.6");
    char summary[1024];

    setup_probe(&probe, &mock, &config, 3u, 4u, 100u, 1000u, 500u, 5000u);
    set_ready_scope(&probe, 3u, 5u);
    add_service(&probe, &mock, "CarPlay", 3u, 10u);
    resolve_service(&probe, &mock, "CarPlay", 3u, 7000u, txt, len, 20u);
    (void)cp_oem_receiver_poll(&probe, 130u, NULL);
    CHECK(cp_oem_receiver_get_snapshot(&probe, &snapshot) == CP_OEM_RECEIVER_OK,
          "snapshot succeeds");
    CHECK(cp_oem_receiver_safe_summary(&snapshot, summary, sizeof(summary)) ==
              CP_OEM_RECEIVER_OK,
          "safe summary succeeds");
    CHECK(strstr(summary, "port=7000") != NULL, "summary includes port");
    CHECK(strstr(summary, "pi_sha256=") != NULL, "summary includes identity fingerprint");
    CHECK(strstr(summary, "deviceid_sha256=") != NULL,
          "summary includes a device id fingerprint");
    CHECK(strstr(summary, "AA:BB:CC:DD:EE:FF") == NULL,
          "summary does not include the device id plaintext");
    CHECK(strstr(summary, "super-secret-identity") == NULL,
          "summary does not include pi plaintext");
    CHECK(strstr(summary, "projection=unavailable") != NULL,
          "summary states projection unavailable");
    CHECK(cp_oem_receiver_safe_summary(&snapshot, summary, 8u) ==
              CP_OEM_RECEIVER_ERR_BOUNDS,
          "short summary buffer rejected");
}

static bool digest_hex_equal(const uint8_t digest[32], const char *hex)
{
    char text[65];
    size_t i;

    for (i = 0u; i < 32u; i++) {
        static const char digits[] = "0123456789abcdef";

        text[i * 2u] = digits[digest[i] >> 4];
        text[i * 2u + 1u] = digits[digest[i] & 0x0fu];
    }
    text[64] = '\0';
    return strcmp(text, hex) == 0;
}

static void test_sha256(void)
{
    uint8_t digest[32];
    uint8_t block[64];
    size_t i;

    CHECK(cp_oem_receiver_sha256("", 0u, digest) == CP_OEM_RECEIVER_OK &&
              digest_hex_equal(
                  digest,
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "SHA-256 empty vector");
    CHECK(cp_oem_receiver_sha256("abc", 3u, digest) == CP_OEM_RECEIVER_OK &&
              digest_hex_equal(
                  digest,
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 abc vector");
    memset(block, (int)'a', sizeof(block));
    CHECK(cp_oem_receiver_sha256(block, 56u, digest) == CP_OEM_RECEIVER_OK &&
              digest_hex_equal(
                  digest,
                  "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"),
          "SHA-256 padding boundary 56");
    CHECK(cp_oem_receiver_sha256(block, 64u, digest) == CP_OEM_RECEIVER_OK &&
              digest_hex_equal(
                  digest,
                  "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"),
          "SHA-256 padding boundary 64");
    for (i = 0u; i < sizeof(block); i++) {
        block[i] = (uint8_t)('a' + (i % 26u));
    }
    CHECK(cp_oem_receiver_sha256(block, sizeof(block), digest) == CP_OEM_RECEIVER_OK,
          "SHA-256 full block accepted");
}

static bool write_all(int fd, const uint8_t *data, size_t len)
{
    size_t offset = 0u;

    while (offset < len) {
        ssize_t written = write(fd, data + offset, len - offset);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static void test_file_pins(void)
{
    char directory[] = "/tmp/opencode/cp-oem-receiver-XXXXXX";
    cp_oem_receiver_file_pin pin = { "fixture.bin",
                                     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" };
    size_t failed = 99u;

    (void)mkdir("/tmp/opencode", 0700);    if (mkdtemp(directory) == NULL) {
        CHECK(false, "mkdtemp fixture directory");
        return;
    }
    {
        char path[256];
        int written_fd;

        (void)snprintf(path, sizeof(path), "%s/fixture.bin", directory);
        written_fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (written_fd < 0) {
            CHECK(false, "create fixture file");
            (void)rmdir(directory);
            return;
        }
        if (!write_all(written_fd, (const uint8_t *)"abc", 3u)) {
            (void)close(written_fd);
            CHECK(false, "write fixture file");
            (void)unlink(path);
            (void)rmdir(directory);
            return;
        }
        (void)close(written_fd);
        CHECK(cp_oem_receiver_verify_files(directory, &pin, 1u, &failed) ==
                  CP_OEM_RECEIVER_OK,
              "matching file pin accepted");
        {
            char alias[256];
            uint8_t digest[CP_OEM_RECEIVER_SHA256_LEN];
            uint8_t sentinel[CP_OEM_RECEIVER_SHA256_LEN];
            memset(sentinel, 0xa5, sizeof(sentinel));
            memcpy(digest, sentinel, sizeof(digest));
            (void)snprintf(alias, sizeof(alias), "%s/alias.bin", directory);
            CHECK(symlink("fixture.bin", alias) == 0, "create pin symlink fixture");
            CHECK(cp_oem_receiver_sha256_file(alias, digest) == CP_OEM_RECEIVER_ERR_IO,
                  "pin hashing refuses symlink even with matching target bytes");
            CHECK(memcmp(digest, sentinel, sizeof(digest)) == 0,
                  "failed symlink hash leaves digest untouched");
            CHECK(unlink(alias) == 0, "remove pin symlink fixture");
            CHECK(symlink("missing.bin", alias) == 0, "create dangling pin symlink fixture");
            CHECK(cp_oem_receiver_sha256_file(alias, digest) == CP_OEM_RECEIVER_ERR_IO,
                  "pin hashing refuses dangling symlink");
            CHECK(unlink(alias) == 0, "remove dangling pin symlink fixture");
            CHECK(mkfifo(alias, 0600) == 0, "create FIFO pin fixture");
            CHECK(cp_oem_receiver_sha256_file(alias, digest) == CP_OEM_RECEIVER_ERR_BOUNDS,
                  "pin hashing refuses FIFO without blocking for a writer");
            CHECK(memcmp(digest, sentinel, sizeof(digest)) == 0,
                  "failed FIFO hash leaves digest untouched");
            CHECK(unlink(alias) == 0, "remove FIFO pin fixture");
            CHECK(cp_oem_receiver_sha256_file(directory, digest) == CP_OEM_RECEIVER_ERR_BOUNDS,
                  "pin hashing refuses directory");
        }
        written_fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (written_fd >= 0) {
            (void)write_all(written_fd, (const uint8_t *)"abd", 3u);
            (void)close(written_fd);
        }
        CHECK(cp_oem_receiver_verify_files(directory, &pin, 1u, &failed) ==
                  CP_OEM_RECEIVER_ERR_PIN_MISMATCH,
              "mismatched file pin rejected");
        CHECK(failed == 0u, "failed pin index reported");
        {
            uint8_t digest[CP_OEM_RECEIVER_SHA256_LEN];
            uint8_t sentinel[CP_OEM_RECEIVER_SHA256_LEN];
            const off_t cap = (off_t)64 * 1024 * 1024;
            memset(sentinel, 0xa5, sizeof(sentinel));
            memcpy(digest, sentinel, sizeof(digest));
            written_fd = open(path, O_WRONLY | O_TRUNC);
            CHECK(written_fd >= 0, "open sparse size-limit fixture");
            CHECK(ftruncate(written_fd, cap + 1) == 0, "create sparse over-cap fixture");
            CHECK(cp_oem_receiver_sha256_file(path, digest) == CP_OEM_RECEIVER_ERR_BOUNDS,
                  "hashing rejects 64 MiB plus one before reading");
            CHECK(memcmp(digest, sentinel, sizeof(digest)) == 0,
                  "over-cap hash leaves digest untouched");
            CHECK(ftruncate(written_fd, cap) == 0, "resize sparse fixture to exact cap");
            CHECK(cp_oem_receiver_sha256_file(path, digest) == CP_OEM_RECEIVER_OK,
                  "hashing accepts exact 64 MiB policy boundary");
            CHECK(close(written_fd) == 0, "close sparse fixture descriptor");
        }
        (void)unlink(path);
    }
    (void)rmdir(directory);
}

int main(void)
{
    test_txt_valid();
    test_txt_missing();
    test_txt_duplicate();
    test_txt_flag_only_unknown();
    test_txt_required_without_value();
    test_txt_malformed();
    test_txt_nul();
    test_txt_invalid_utf8();
    test_txt_too_long();
    test_srv_validation();
    test_scope();
    test_observation_without_scope();
    test_observation_unverified();
    test_local_attestation_ready();
    test_local_attestation_mismatch();
    test_interface_change();
    test_removal();
    test_changed_port();
    test_timeout();
    test_resolve_timeout();
    test_too_many_services();
    test_ambiguous();
    test_stale();
    test_source_version_mismatch();
    test_remote_interface_ignored();
    test_port_zero_rejected();
    test_unsolicited_resolve();
    test_dns_error();
    test_start_failure();
    test_safe_summary();
    test_sha256();
    test_file_pins();

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all OEM receiver probe tests passed\n");
    return 0;
}
