/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Independent wire-vector and state-machine tests for cp_link.
 * Expected frames are written as fixed bytes or built by the test's own
 * checksum/frame encoder, never by calling the engine's internal encoder.
 */
#include "../src/carplay/cp_link.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define MAX_EVENTS 128u
#define MAX_CAPTURE_DATA CP_LINK_MAX_PAYLOAD

typedef struct capture_event {
    cp_link_event_type_t type;
    cp_link_state_t state;
    bool writable;
    cp_link_reset_reason_t reason;
    uint32_t generation;
    uint8_t session_id;
    uint8_t session_type;
    uint16_t len;
    uint8_t data[MAX_CAPTURE_DATA];
} capture_event_t;

static cp_link_t g_link;
static capture_event_t g_events[MAX_EVENTS];
static size_t g_event_count;

static void capture_event(void *user, const cp_link_event_t *event)
{
    capture_event_t *out;

    (void)user;
    if (g_event_count >= MAX_EVENTS) {
        fprintf(stderr, "FAIL: event capture overflow\n");
        exit(1);
    }
    out = &g_events[g_event_count++];
    memset(out, 0, sizeof(*out));
    out->type = event->type;
    out->state = event->state;
    out->writable = event->writable;
    out->reason = event->reset_reason;
    out->generation = event->generation;
    out->session_id = event->session_id;
    out->session_type = event->session_type;
    out->len = event->data_len;
    if (event->data != NULL && event->data_len <= MAX_CAPTURE_DATA)
        memcpy(out->data, event->data, event->data_len);
}

static void clear_events(void)
{
    g_event_count = 0;
}

static size_t count_events(cp_link_event_type_t type)
{
    size_t count = 0;
    size_t i;

    for (i = 0; i < g_event_count; i++) {
        if (g_events[i].type == type)
            count++;
    }
    return count;
}

static capture_event_t *last_event(cp_link_event_type_t type)
{
    size_t i = g_event_count;

    while (i > 0) {
        i--;
        if (g_events[i].type == type)
            return &g_events[i];
    }
    return NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static size_t hex2bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int high = -1;

    while (*hex != '\0') {
        int value;

        if (*hex == ' ' || *hex == '\t' || *hex == '\n' || *hex == '\r') {
            hex++;
            continue;
        }
        value = hex_value(*hex++);
        CHECK(value >= 0);
        if (high < 0) {
            high = value;
        } else {
            CHECK(n < cap);
            out[n++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    CHECK(high < 0);
    return n;
}

static void print_hex(const char *label, const uint8_t *data, size_t len)
{
    size_t i;

    fprintf(stderr, "%s (%zu):", label, len);
    for (i = 0; i < len; i++)
        fprintf(stderr, " %02x", data[i]);
    fprintf(stderr, "\n");
}

static size_t drain_all(cp_link_t *link, uint8_t *out, size_t cap)
{
    size_t total = 0;

    for (;;) {
        const uint8_t *part = NULL;
        size_t len = cp_link_output_peek(link, &part);

        if (len == 0)
            break;
        CHECK(part != NULL);
        CHECK(total + len <= cap);
        memcpy(out + total, part, len);
        CHECK(cp_link_output_consume(link, len) == CP_LINK_OK);
        total += len;
    }
    return total;
}

static void expect_wire(cp_link_t *link, const char *hex, const char *what)
{
    uint8_t expected[CP_LINK_MAX_FRAME * 2u];
    uint8_t got[CP_LINK_MAX_FRAME * 2u];
    size_t expected_len = hex2bin(hex, expected, sizeof(expected));
    size_t got_len = drain_all(link, got, sizeof(got));

    if (expected_len != got_len ||
        memcmp(expected, got, expected_len) != 0) {
        fprintf(stderr, "wire mismatch: %s\n", what);
        print_hex("expected", expected, expected_len);
        print_hex("got", got, got_len);
        CHECK(0);
    }
}

static void expect_wire_bytes(cp_link_t *link, const uint8_t *expected,
                              size_t expected_len, const char *what)
{
    uint8_t got[CP_LINK_MAX_FRAME * 4u];
    size_t got_len = drain_all(link, got, sizeof(got));

    if (expected_len != got_len ||
        memcmp(expected, got, expected_len) != 0) {
        fprintf(stderr, "wire mismatch: %s\n", what);
        print_hex("expected", expected, expected_len);
        print_hex("got", got, got_len);
        CHECK(0);
    }
}

/* Compare the oldest queued frame without consuming it. */
static void expect_peek_bytes(cp_link_t *link, const uint8_t *expected,
                              size_t expected_len, const char *what)
{
    const uint8_t *got = NULL;
    size_t got_len = cp_link_output_peek(link, &got);

    if (expected_len != got_len || got == NULL ||
        memcmp(expected, got, expected_len) != 0) {
        fprintf(stderr, "queued wire mismatch: %s\n", what);
        print_hex("expected", expected, expected_len);
        if (got != NULL)
            print_hex("got", got, got_len);
        else
            fprintf(stderr, "got (0):\n");
        CHECK(0);
    }
}

static void expect_no_output(cp_link_t *link)
{
    const uint8_t *data = NULL;

    CHECK(cp_link_output_peek(link, &data) == 0);
    CHECK(cp_link_output_pending(link) == 0);
}

/* Scan a concatenated drained stream for a frame with `control`; the stream
 * must consist only of complete frames. */
static bool drained_has_control(const uint8_t *data, size_t len,
                                uint8_t control, size_t *frame_len)
{
    size_t offset = 0;

    if (frame_len != NULL)
        *frame_len = 0;
    while (offset < len) {
        size_t length;

        CHECK(len - offset >= CP_LINK_HEADER_LEN);
        length = ((size_t)data[offset + 2u] << 8) | data[offset + 3u];
        CHECK(length >= CP_LINK_HEADER_LEN);
        CHECK(length <= len - offset);
        if (data[offset + 4u] == control) {
            if (frame_len != NULL)
                *frame_len = length;
            return true;
        }
        offset += length;
    }
    return false;
}

static size_t make_frame(uint8_t *out, size_t cap, uint8_t control,
                         uint8_t seq, uint8_t ack, uint8_t session_id,
                         const uint8_t *payload, size_t payload_len,
                         bool has_payload)
{
    size_t total = 9u + (has_payload ? payload_len + 1u : 0u);
    uint8_t sum = 0;
    size_t i;

    CHECK(total <= cap);
    out[0] = 0xFF;
    out[1] = 0x5A;
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)(total & 0xFFu);
    out[4] = control;
    out[5] = seq;
    out[6] = ack;
    out[7] = session_id;
    for (i = 0; i < 8u; i++)
        sum = (uint8_t)(sum + out[i]);
    out[8] = (uint8_t)((256u - (unsigned)sum) & 0xFFu);
    if (has_payload) {
        if (payload_len > 0u) {
            CHECK(payload != NULL);
            memcpy(out + 9u, payload, payload_len);
        }
        sum = 0;
        for (i = 0; i < payload_len; i++)
            sum = (uint8_t)(sum + payload[i]);
        out[9u + payload_len] = (uint8_t)((256u - (unsigned)sum) & 0xFFu);
    }
    return total;
}

static void resign_header(uint8_t *frame)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < 8u; i++)
        sum = (uint8_t)(sum + frame[i]);
    frame[8] = (uint8_t)((256u - (unsigned)sum) & 0xFFu);
}

static void fill_sequence(uint8_t *out, size_t count, uint8_t first)
{
    size_t i;

    for (i = 0; i < count; i++)
        out[i] = (uint8_t)(first + i);
}

static void feed_bytes(cp_link_t *link, const uint8_t *data, size_t len,
                       uint64_t now_ms)
{
    CHECK(cp_link_feed(link, data, len, now_ms) == CP_LINK_OK);
}

static void feed_frame(cp_link_t *link, const uint8_t *frame, size_t len,
                       uint64_t now_ms)
{
    feed_bytes(link, frame, len, now_ms);
}

static void base_config(cp_link_config_t *cfg)
{
    cp_link_config_default(cfg);
    cfg->initial_seq = 0x63;
    cfg->control_session_id = 10;
    cfg->external_accessory_session_id = 11;
    cfg->file_transfer_session_id = 12;
    cfg->control_version = 1;
    cfg->max_outgoing = 4;
    cfg->max_payload = 64;
    cfg->retransmission_timeout_ms = 1000;
    cfg->ack_timeout_ms = 100;
    cfg->max_retransmissions = 3;
    cfg->max_ack = 2;
    cfg->detect_interval_ms = 1000;
    cfg->detect_retry_limit = 2;
    cfg->negotiate_interval_ms = 500;
    cfg->negotiate_retry_limit = 2;
    cfg->handshake_timeout_ms = 10000;
}

static size_t build_local_lsp(const cp_link_config_t *cfg, uint8_t *out,
                              size_t cap)
{
    size_t n = 0;
    uint16_t retx = cfg->zero_ack ? 0u : cfg->retransmission_timeout_ms;
    uint16_t ack_timeout = cfg->zero_ack ? 0u : cfg->ack_timeout_ms;
    uint8_t max_retx = cfg->zero_ack ? 0u : cfg->max_retransmissions;
    uint8_t max_ack = cfg->zero_ack ? 0u : cfg->max_ack;
    uint16_t max_len = (uint16_t)(cfg->max_payload + 10u);

    CHECK(10u + 3u * 3u <= cap);
    out[n++] = 0x01;
    out[n++] = cfg->max_outgoing;
    out[n++] = (uint8_t)(max_len >> 8);
    out[n++] = (uint8_t)(max_len & 0xFFu);
    out[n++] = (uint8_t)(retx >> 8);
    out[n++] = (uint8_t)(retx & 0xFFu);
    out[n++] = (uint8_t)(ack_timeout >> 8);
    out[n++] = (uint8_t)(ack_timeout & 0xFFu);
    out[n++] = max_retx;
    out[n++] = max_ack;
    out[n++] = cfg->control_session_id;
    out[n++] = 0x00;
    out[n++] = cfg->control_version;
    if (cfg->external_accessory_session_id != 0u) {
        out[n++] = cfg->external_accessory_session_id;
        out[n++] = 0x02;
        out[n++] = 0x01;
    }
    if (cfg->file_transfer_session_id != 0u) {
        out[n++] = cfg->file_transfer_session_id;
        out[n++] = 0x01;
        out[n++] = 0x02;
    }
    return n;
}

static size_t build_peer_lsp(uint8_t *out, size_t cap, uint8_t max_outgoing,
                             uint16_t max_len, uint16_t retx, uint16_t ack_to,
                             uint8_t max_retx, uint8_t max_ack,
                             const uint8_t *sessions, size_t session_bytes)
{
    size_t need = 10u + session_bytes;

    CHECK(need <= cap);
    out[0] = 0x01;
    out[1] = max_outgoing;
    out[2] = (uint8_t)(max_len >> 8);
    out[3] = (uint8_t)(max_len & 0xFFu);
    out[4] = (uint8_t)(retx >> 8);
    out[5] = (uint8_t)(retx & 0xFFu);
    out[6] = (uint8_t)(ack_to >> 8);
    out[7] = (uint8_t)(ack_to & 0xFFu);
    out[8] = max_retx;
    out[9] = max_ack;
    if (session_bytes > 0u) {
        CHECK(sessions != NULL);
        memcpy(out + 10u, sessions, session_bytes);
    }
    return need;
}

static void expected_local_syn(const cp_link_config_t *cfg, uint8_t *frame,
                               size_t *frame_len)
{
    uint8_t lsp[10u + 3u * 3u];
    size_t lsp_len = build_local_lsp(cfg, lsp, sizeof(lsp));

    *frame_len = make_frame(frame, CP_LINK_MAX_FRAME, 0x80, cfg->initial_seq,
                            0x00, 0x00, lsp, lsp_len, true);
}

static void init_start_passive(cp_link_t *link, const cp_link_config_t *cfg)
{
    CHECK(cp_link_init(link, cfg, capture_event, NULL) == CP_LINK_OK);
    clear_events();
    CHECK(cp_link_start(link, CP_LINK_ROLE_PASSIVE, 0) == CP_LINK_OK);
    CHECK(cp_link_state(link) == CP_LINK_STATE_DETECT);
    expect_wire(link, "ff550200ee10", "passive detection marker");
}

static void handshake_with_peer_lsp(cp_link_t *link,
                                    const cp_link_config_t *cfg,
                                    const uint8_t *peer_syn,
                                    size_t peer_syn_len)
{
    uint8_t local_syn[CP_LINK_MAX_FRAME];
    uint8_t expected_ack[CP_LINK_HEADER_LEN];
    uint8_t ack_for_local[CP_LINK_HEADER_LEN];
    uint8_t got[CP_LINK_MAX_FRAME];
    size_t local_syn_len;
    size_t expected_ack_len;
    size_t ack_for_local_len;
    size_t got_len;

    CHECK(peer_syn != NULL && peer_syn_len >= CP_LINK_HEADER_LEN);
    init_start_passive(link, cfg);
    feed_bytes(link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
    expected_local_syn(cfg, local_syn, &local_syn_len);
    got_len = drain_all(link, got, sizeof(got));
    CHECK(got_len == local_syn_len);
    CHECK(memcmp(got, local_syn, local_syn_len) == 0);

    feed_frame(link, peer_syn, peer_syn_len, 0);
    expected_ack_len = make_frame(expected_ack, sizeof(expected_ack), 0x40,
                                  cfg->initial_seq, peer_syn[5], 0, NULL, 0,
                                  false);
    got_len = drain_all(link, got, sizeof(got));
    if (got_len != expected_ack_len) {
        print_hex("peer syn", peer_syn, peer_syn_len);
        print_hex("expected ack", expected_ack, expected_ack_len);
        print_hex("got ack", got, got_len);
    }
    CHECK(got_len == expected_ack_len);
    CHECK(memcmp(got, expected_ack, expected_ack_len) == 0);

    ack_for_local_len = make_frame(ack_for_local, sizeof(ack_for_local), 0x40,
                                   0, cfg->initial_seq, 0, NULL, 0, false);
    feed_frame(link, ack_for_local, ack_for_local_len, 0);
    CHECK(cp_link_state(link) == CP_LINK_STATE_NORMAL);
    clear_events();
}

/* Default peer SYN (0x2a) with control id 3, EA id 5, FT id 9. */
static const char *PEER_SYN_2A =
    "ff5a001d802a0000e0"
    "0108040003e800640302030001050201090102"
    "87";

static void handshake_default(cp_link_t *link)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    size_t peer_len;

    base_config(&cfg);
    peer_len = hex2bin(PEER_SYN_2A, peer_syn, sizeof(peer_syn));
    handshake_with_peer_lsp(link, &cfg, peer_syn, peer_len);
}

static void test_marker_and_local_syn_vector(void)
{
    cp_link_config_t cfg;
    const cp_link_sync_t *local;

    base_config(&cfg);
    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    clear_events();
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_INITIATOR, 0) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DETECT);
    CHECK(cp_link_generation(&g_link) == 1u);
    CHECK(cp_link_writable(&g_link) == false);
    CHECK(cp_link_next_deadline(&g_link) == 500u);

    expect_wire(&g_link,
                "ff550200ee10"
                "ff5a001d80630000a7"
                "0104004a03e8006403020a00010b02010c0102"
                "35",
                "initiator marker and local SYN");

    local = cp_link_local_sync(&g_link);
    CHECK(local != NULL);
    CHECK(local->version == 1u);
    CHECK(local->max_outgoing == 4u);
    CHECK(local->max_len == 74u);
    CHECK(local->retransmission_timeout_ms == 1000u);
    CHECK(local->ack_timeout_ms == 100u);
    CHECK(local->max_retransmissions == 3u);
    CHECK(local->max_ack == 2u);
    CHECK(local->session_count == 3u);
    CHECK(local->sessions[0].id == 10u &&
          local->sessions[0].type == CP_LINK_SESSION_CONTROL);
    CHECK(local->sessions[1].id == 11u &&
          local->sessions[1].type == CP_LINK_SESSION_EXTERNAL_ACCESSORY);
    CHECK(local->sessions[2].id == 12u &&
          local->sessions[2].type == CP_LINK_SESSION_FILE_TRANSFER);
    CHECK(cp_link_stats(&g_link)->marker_sent == 1u);
    CHECK(cp_link_stats(&g_link)->syn_sent == 1u);
    puts("PASS: detection marker and fixed local SYN vector");
}

static void test_reference_ack_vector(void)
{
    cp_link_config_t cfg;
    uint8_t lsp[64];
    uint8_t peer_syn[128];
    uint8_t local_syn[CP_LINK_MAX_FRAME];
    const uint8_t sessions[] = {3, 0, 1};
    size_t lsp_len;
    size_t peer_len;
    size_t local_len;

    base_config(&cfg);
    lsp_len = build_peer_lsp(lsp, sizeof(lsp), 8, 1024, 1000, 100, 3, 2,
                             sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x05, 0, 0, lsp,
                          lsp_len, true);
    init_start_passive(&g_link, &cfg);
    feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
    expected_local_syn(&cfg, local_syn, &local_len);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));

        CHECK(got_len == local_len);
        CHECK(memcmp(got, local_syn, local_len) == 0);
    }
    feed_frame(&g_link, peer_syn, peer_len, 0);
    /* Reference iap2-link test vector: length 9, ACK, seq 0x63, ack 0x05. */
    expect_wire(&g_link, "ff5a000940630500f6",
                "reference ACK header vector");
    puts("PASS: reference ACK header vector");
}

static void test_passive_syn_fields_and_session_direction(void)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    uint8_t inbound[CP_LINK_MAX_FRAME];
    size_t peer_len;
    size_t inbound_len;
    const cp_link_sync_t *peer;
    capture_event_t *event;
    const uint8_t payload[] = {0x01, 0x02, 0x03};
    const uint8_t csm[] = {0x40, 0x40};

    base_config(&cfg);
    peer_len = hex2bin(PEER_SYN_2A, peer_syn, sizeof(peer_syn));
    init_start_passive(&g_link, &cfg);

    feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
    expect_wire(&g_link,
                "ff5a001d80630000a7"
                "0104004a03e8006403020a00010b02010c0102"
                "35",
                "passive local SYN");

    feed_frame(&g_link, peer_syn, peer_len, 0);
    expect_wire(&g_link, "ff5a000940632a00d1", "ACK for peer SYN seq 0x2a");
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NEGOTIATE);
    CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_CONTROL) == 3);
    CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_EXTERNAL_ACCESSORY) == 5);
    CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_FILE_TRANSFER) == 9);
    CHECK(cp_link_session_id(&g_link, 7u) == CP_LINK_SESSION_ID_NONE);

    peer = cp_link_peer_sync(&g_link);
    CHECK(peer != NULL);
    CHECK(peer->version == 1u);
    CHECK(peer->max_outgoing == 8u);
    CHECK(peer->max_len == 1024u);
    CHECK(peer->retransmission_timeout_ms == 1000u);
    CHECK(peer->ack_timeout_ms == 100u);
    CHECK(peer->max_retransmissions == 3u);
    CHECK(peer->max_ack == 2u);
    CHECK(peer->session_count == 3u);
    CHECK(peer->sessions[0].id == 3u &&
          peer->sessions[0].type == CP_LINK_SESSION_CONTROL &&
          peer->sessions[0].version == 1u);
    CHECK(peer->sessions[1].id == 5u &&
          peer->sessions[1].type == CP_LINK_SESSION_EXTERNAL_ACCESSORY);
    CHECK(peer->sessions[2].id == 9u &&
          peer->sessions[2].type == CP_LINK_SESSION_FILE_TRANSFER);

    feed_frame(&g_link,
               (const uint8_t *)"\xff\x5a\x00\x09\x40\x00\x63\x00\xfb", 9, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_writable(&g_link));
    CHECK(cp_link_generation(&g_link) == 1u);
    clear_events();
    expect_no_output(&g_link);

    /* Outbound control must use the peer's negotiated id 3, never 10. */
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, payload,
                       sizeof(payload), 0) == CP_LINK_OK);
    expect_wire(&g_link, "ff5a000d40642a03c9010203fa",
                "control data uses negotiated session 3");

    /* Inbound control is reported raw; no CSM parsing or re-framing. */
    inbound_len = make_frame(inbound, sizeof(inbound), 0x40, 0x2b, 0x64, 3,
                             csm, sizeof(csm), true);
    feed_frame(&g_link, inbound, inbound_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    event = last_event(CP_LINK_EVENT_SESSION_DATA);
    CHECK(event != NULL);
    CHECK(event->session_id == 3u);
    CHECK(event->session_type == CP_LINK_SESSION_CONTROL);
    CHECK(event->len == sizeof(csm));
    CHECK(memcmp(event->data, csm, sizeof(csm)) == 0);
    CHECK(cp_link_generation(&g_link) == 1u);
    CHECK(cp_link_next_deadline(&g_link) == 100u);
    expect_no_output(&g_link);
    CHECK(cp_link_tick(&g_link, 99u) == CP_LINK_OK);
    expect_no_output(&g_link);
    CHECK(cp_link_tick(&g_link, 100u) == CP_LINK_OK);
    expect_wire(&g_link, "ff5a000940642b00cf", "delayed cumulative ACK");

    /* A legacy hard-coded id 10 is not the negotiated control session. */
    inbound_len = make_frame(inbound, sizeof(inbound), 0x40, 0x2c, 0x64, 10,
                             csm, sizeof(csm), true);
    feed_frame(&g_link, inbound, inbound_len, 100);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    CHECK(cp_link_stats(&g_link)->rx_unknown_session == 1u);

    /* The other negotiated ids are used on their respective sessions too. */
    {
        uint8_t out[CP_LINK_MAX_FRAME];
        size_t out_len;

        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_EXTERNAL_ACCESSORY,
                           payload, 1, 100) == CP_LINK_OK);
        out_len = drain_all(&g_link, out, sizeof(out));
        CHECK(out_len == 11u && out[7] == 5u);
        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_FILE_TRANSFER,
                           payload, 1, 100) == CP_LINK_OK);
        out_len = drain_all(&g_link, out, sizeof(out));
        CHECK(out_len == 11u && out[7] == 9u);
    }
    puts("PASS: SYN fields, negotiated session direction and raw CSM payload");
}

static void test_unsupported_syn_sessions(void)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    uint8_t lsp[64];
    uint8_t frame[CP_LINK_MAX_FRAME];
    const uint8_t sessions[] = {3, 0, 1, 4, 9, 7};
    const uint8_t data[] = {0xDE, 0xAD};
    size_t peer_len;
    size_t lsp_len;
    size_t frame_len;
    capture_event_t *event;

    base_config(&cfg);
    lsp_len = build_peer_lsp(lsp, sizeof(lsp), 8, 1024, 1000, 100, 3, 2,
                             sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0, lsp,
                          lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_CONTROL) == 3);
    CHECK(cp_link_session_id(&g_link, 9u) == CP_LINK_SESSION_ID_NONE);
    CHECK(cp_link_peer_sync(&g_link)->session_count == 2u);

    clear_events();
    CHECK(cp_link_send(&g_link, 9u, data, sizeof(data), 0) ==
          CP_LINK_ERR_NO_SESSION);
    CHECK(cp_link_send_session(&g_link, 99u, data, sizeof(data), 0) ==
          CP_LINK_ERR_NO_SESSION);
    CHECK(cp_link_send_session(&g_link, 4u, data, sizeof(data), 0) ==
          CP_LINK_OK);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));

        CHECK(got_len > 9u);
        CHECK(got[7] == 4u);
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x64, 4, data,
                           sizeof(data), true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    event = last_event(CP_LINK_EVENT_SESSION_DATA);
    CHECK(event != NULL);
    CHECK(event->session_id == 4u);
    CHECK(event->session_type == 9u);
    CHECK(event->len == sizeof(data));

    /* Data on an id absent from the peer SYN advances the link but is not
     * surfaced as application data. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x64, 99, data,
                           sizeof(data), true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    CHECK(cp_link_stats(&g_link)->rx_unknown_session == 1u);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2d, 0x64, 3, data,
                           sizeof(data), true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 2u);

    /* A SYN without a control session is an explicit link failure. */
    {
        const uint8_t no_control_sessions[] = {5, 2, 1};
        uint8_t syn[128];

        lsp_len = build_peer_lsp(lsp, sizeof(lsp), 8, 1024, 1000, 100, 3, 2,
                                 no_control_sessions,
                                 sizeof(no_control_sessions));
        peer_len = make_frame(syn, sizeof(syn), 0x80, 0x2a, 0, 0, lsp, lsp_len,
                              true);
        init_start_passive(&g_link, &cfg);
        feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
        {
            uint8_t local_syn[CP_LINK_MAX_FRAME];
            size_t local_len;

            expected_local_syn(&cfg, local_syn, &local_len);
            {
                uint8_t got[CP_LINK_MAX_FRAME];
                size_t got_len = drain_all(&g_link, got, sizeof(got));

                CHECK(got_len == local_len);
            }
        }
        feed_frame(&g_link, syn, peer_len, 0);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
        CHECK(cp_link_reset_reason(&g_link) ==
              CP_LINK_RESET_SESSION_UNSUPPORTED);
        CHECK(cp_link_generation(&g_link) == 2u);
        CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_CONTROL) ==
              CP_LINK_SESSION_ID_NONE);
        expect_no_output(&g_link);
        CHECK(last_event(CP_LINK_EVENT_RESET) != NULL);
        CHECK(last_event(CP_LINK_EVENT_RESET)->reason ==
              CP_LINK_RESET_SESSION_UNSUPPORTED);
    }

    /* A peer window larger than the fixed tx storage is an explicit limit. */
    {
        const uint8_t control_only[] = {3, 0, 1};
        uint8_t syn[128];

        lsp_len = build_peer_lsp(lsp, sizeof(lsp), 33, 1024, 1000, 100, 3, 2,
                                 control_only, sizeof(control_only));
        peer_len = make_frame(syn, sizeof(syn), 0x80, 0x2a, 0, 0, lsp, lsp_len,
                              true);
        init_start_passive(&g_link, &cfg);
        feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
        {
            uint8_t local_syn[CP_LINK_MAX_FRAME];
            size_t local_len;

            expected_local_syn(&cfg, local_syn, &local_len);
            {
                uint8_t got[CP_LINK_MAX_FRAME];
                size_t got_len = drain_all(&g_link, got, sizeof(got));

                CHECK(got_len == local_len);
            }
        }
        feed_frame(&g_link, syn, peer_len, 0);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
        CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_LIMITS);
        CHECK(cp_link_generation(&g_link) == 2u);
        expect_no_output(&g_link);
    }
    puts("PASS: unknown session types ignored by name lookup; no-control/oversize SYN rejected");
}

static void test_zero_session_is_sequenced(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_HEADER_LEN];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t unknown = 0x99;
    const uint8_t known = 0xAB;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    capture_event_t *event;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 8, 1024, 1000, 100,
                             3, 1, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Session id zero is the link session, never a data session. It is still
     * sequenced and ACKed so the next valid frame is not mistaken for a gap. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 0,
                           &unknown, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    CHECK(cp_link_stats(&g_link)->rx_unknown_session == 1u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x63, 3,
                           &known, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    event = last_event(CP_LINK_EVENT_SESSION_DATA);
    CHECK(event != NULL && event->len == 1u && event->data[0] == known);
    CHECK(cp_link_stats(&g_link)->data_received == 1u);

    /* max_ack is one, so the cumulative ACK advances over both frames and
     * coalesces into a single queued frame. */
    expected_len = make_frame(expected, sizeof(expected), 0x40, 0x63, 0x2c,
                              0, NULL, 0, false);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));

        CHECK(got_len == expected_len);
        CHECK(memcmp(got, expected, expected_len) == 0);
    }
    puts("PASS: zero-session frame advances sequence and the next valid frame is delivered");
}

static void test_corruption_checksums_and_lengths(void)
{
    cp_link_config_t cfg;
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t bad[CP_LINK_MAX_FRAME];
    uint8_t valid_payload[] = {'o', 'k'};
    size_t frame_len;
    capture_event_t *event;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();

    /* Bad header checksum, then a good frame. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           valid_payload, sizeof(valid_payload), true);
    memcpy(bad, frame, frame_len);
    bad[8] ^= 0x01u;
    feed_bytes(&g_link, bad, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    CHECK(cp_link_stats(&g_link)->frames_bad_header >= 1u);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    event = last_event(CP_LINK_EVENT_SESSION_DATA);
    CHECK(event != NULL && event->len == sizeof(valid_payload) &&
          memcmp(event->data, valid_payload, sizeof(valid_payload)) == 0);
    clear_events();

    /* Bad payload checksum, then the same sequence number again. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x63, 3,
                           valid_payload, sizeof(valid_payload), true);
    memcpy(bad, frame, frame_len);
    bad[frame_len - 1u] ^= 0x80u;
    feed_bytes(&g_link, bad, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    CHECK(cp_link_stats(&g_link)->frames_bad_payload >= 1u);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    clear_events();

    /* Length smaller than the 9-byte header, valid checksum. */
    frame_len = make_frame(bad, sizeof(bad), 0x40, 0x2d, 0x63, 3, NULL, 0,
                           false);
    bad[2] = 0x00u;
    bad[3] = 0x08u;
    {
        uint8_t sum = 0;
        size_t i;

        for (i = 0; i < 8u; i++)
            sum = (uint8_t)(sum + bad[i]);
        bad[8] = (uint8_t)((256u - (unsigned)sum) & 0xFFu);
    }
    feed_bytes(&g_link, bad, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    CHECK(cp_link_stats(&g_link)->frames_bad_length >= 1u);

    /* Oversized length (> local max packet), valid header checksum. */
    frame_len = make_frame(bad, sizeof(bad), 0x40, 0x2d, 0x63, 3, NULL, 0,
                           false);
    bad[2] = 0x00u;
    bad[3] = (uint8_t)(cfg.max_payload + 11u);
    {
        uint8_t sum = 0;
        size_t i;

        for (i = 0; i < 8u; i++)
            sum = (uint8_t)(sum + bad[i]);
        bad[8] = (uint8_t)((256u - (unsigned)sum) & 0xFFu);
    }
    feed_bytes(&g_link, bad, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    CHECK(cp_link_stats(&g_link)->frames_bad_length >= 2u);

    /* The parser must still accept a good frame after the junk. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2d, 0x63, 3,
                           valid_payload, sizeof(valid_payload), true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: header/payload checksum and invalid length corruption is never delivered");
}

static void test_fragmentation_coalescing_noise(void)
{
    cp_link_config_t cfg;
    uint8_t f1[64];
    uint8_t f2[64];
    uint8_t stream[256];
    uint8_t expected1[] = {'o', 'n', 'e'};
    uint8_t expected2[] = {'t', 'w', 'o'};
    size_t f1_len;
    size_t f2_len;
    size_t stream_len = 0;
    size_t i;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();

    f1_len = make_frame(f1, sizeof(f1), 0x40, 0x2b, 0x63, 3, expected1,
                        sizeof(expected1), true);
    f2_len = make_frame(f2, sizeof(f2), 0x40, 0x2c, 0x63, 3, expected2,
                        sizeof(expected2), true);
    stream[stream_len++] = 0xDE;
    stream[stream_len++] = 0xAD;
    memcpy(stream + stream_len, f1, f1_len);
    stream_len += f1_len;
    stream[stream_len++] = 0x00;
    memcpy(stream + stream_len, f2, f2_len);
    stream_len += f2_len;

    for (i = 0; i < stream_len; i++)
        feed_bytes(&g_link, &stream[i], 1u, 0);

    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 2u);
    CHECK(g_events[0].len == sizeof(expected1));
    CHECK(memcmp(g_events[0].data, expected1, sizeof(expected1)) == 0);
    CHECK(g_events[1].len == sizeof(expected2));
    CHECK(memcmp(g_events[1].data, expected2, sizeof(expected2)) == 0);
    CHECK(cp_link_stats(&g_link)->resync_bytes >= 2u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: one-byte fragmentation, coalescing and leading noise resync");
}

static void test_partial_output_and_backpressure(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    const uint8_t sessions[] = {3, 0, 1, 5, 2, 1, 9, 1, 2};
    const uint8_t data = 0x11;
    const uint8_t *part = NULL;
    size_t lsp_len;
    size_t peer_len;
    size_t len;
    unsigned accepted = 0;
    unsigned i;

    base_config(&cfg);
    cfg.max_outgoing = 30;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 30, 1024, 1000, 100,
                             3, 2, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);

    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    clear_events();
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_INITIATOR, 0) == CP_LINK_OK);

    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 6u && part != NULL && memcmp(part, "\xff\x55\x02\x00\xee\x10", 6) == 0);
    CHECK(cp_link_output_consume(&g_link, 2u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 4u && memcmp(part, "\x02\x00\xee\x10", 4) == 0);
    CHECK(cp_link_output_consume(&g_link, 4u) == CP_LINK_OK);

    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 29u);
    CHECK(cp_link_output_consume(&g_link, 3u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 26u && part[0] == 0x1Du && part[1] == 0x80u);
    CHECK(cp_link_output_consume(&g_link, 100u) == CP_LINK_ERR_INVALID);
    CHECK(cp_link_output_consume(&g_link, 26u) == CP_LINK_OK);
    expect_no_output(&g_link);

    feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
    feed_frame(&g_link, peer_syn, peer_len, 0);
    feed_frame(&g_link, (const uint8_t *)"\xff\x5a\x00\x09\x40\x00\x63\x00\xfb", 9, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    clear_events();

    for (i = 0; i < 20u; i++) {
        int rc = cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &data, 1u, 0);

        if (rc == CP_LINK_OK)
            accepted++;
        else
            CHECK(rc == CP_LINK_ERR_BACKPRESSURE);
    }
    CHECK(accepted == 10u);
    CHECK(cp_link_writable(&g_link) == false);
    CHECK(cp_link_stats(&g_link)->data_sent == accepted);
    CHECK(cp_link_stats(&g_link)->tx_backpressure >= 1u);
    CHECK(cp_link_output_pending(&g_link) == accepted * 11u);

    /* A partial frame still occupies its queued slot. */
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 11u);
    CHECK(cp_link_output_consume(&g_link, 5u) == CP_LINK_OK);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &data, 1u, 0) ==
          CP_LINK_ERR_BACKPRESSURE);
    CHECK(cp_link_output_consume(&g_link, 6u) == CP_LINK_OK);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &data, 1u, 0) ==
          CP_LINK_OK);
    CHECK(cp_link_stats(&g_link)->data_sent == accepted + 1u);
    puts("PASS: partial output writes and bounded tx backpressure");
}

static void test_ack_burst_coalesces(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t stream[512];
    uint8_t out[128];
    uint8_t expected[CP_LINK_HEADER_LEN];
    const uint8_t sessions[] = {3, 0, 1, 5, 2, 1};
    const uint8_t payload = 0x5A;
    size_t lsp_len;
    size_t peer_len;
    size_t stream_len = 0;
    size_t out_len;
    size_t expected_len;
    unsigned i;

    base_config(&cfg);
    cfg.max_outgoing = 8;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 8, 1024, 1000, 100,
                             3, 1, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Twenty one-byte frames arrive in a single coalesced feed. max_ack is
     * one, so every frame reaches an ACK opportunity while the caller has no
     * chance to drain. One ACK slot must absorb all of them. */
    for (i = 0; i < 20u; i++) {
        uint8_t frame[32];
        size_t frame_len;

        frame_len = make_frame(frame, sizeof(frame), 0x40,
                               (uint8_t)(0x2bu + i), 0x63, 3, &payload, 1u,
                               true);
        CHECK(stream_len + frame_len <= sizeof(stream));
        memcpy(stream + stream_len, frame, frame_len);
        stream_len += frame_len;
    }
    feed_bytes(&g_link, stream, stream_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_generation(&g_link) == 1u);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    CHECK(cp_link_stats(&g_link)->data_received == 20u);
    CHECK(cp_link_output_pending(&g_link) == CP_LINK_HEADER_LEN);

    expected_len = make_frame(expected, sizeof(expected), 0x40, 0x63,
                              (uint8_t)(0x2au + 20u), 0, NULL, 0, false);
    out_len = drain_all(&g_link, out, sizeof(out));
    CHECK(out_len == expected_len);
    CHECK(memcmp(out, expected, expected_len) == 0);
    puts("PASS: 20 ACK opportunities coalesce into one cumulative ACK frame");
}

static void test_partially_consumed_ack_is_immutable(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_HEADER_LEN];
    uint8_t prefix[CP_LINK_HEADER_LEN];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x66;
    const uint8_t *part = NULL;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    size_t len;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 8, 1024, 1000, 100,
                             3, 1, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_output_pending(&g_link) == CP_LINK_HEADER_LEN);
    CHECK(cp_link_output_consume(&g_link, 4u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 5u && part != NULL);
    memcpy(prefix, part, len);

    /* The tail ACK has an emitted prefix and must not be rewritten. A new
     * ACK is queued behind its remaining five bytes. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    CHECK(cp_link_output_pending(&g_link) == 5u + CP_LINK_HEADER_LEN);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 5u && memcmp(part, prefix, len) == 0);
    CHECK(prefix[2] == 0x2bu); /* the old emitted ACK value is untouched */
    CHECK(cp_link_output_consume(&g_link, 5u) == CP_LINK_OK);

    expected_len = make_frame(expected, sizeof(expected), 0x40, 0x63, 0x2c,
                              0, NULL, 0, false);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == expected_len);
    CHECK(memcmp(part, expected, expected_len) == 0);
    CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
    expect_no_output(&g_link);
    puts("PASS: partially consumed ACK is immutable; a fresh ACK follows it");
}

static void test_full_output_ack_deferred(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t eak[CP_LINK_MAX_FRAME];
    uint8_t out[512];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x77;
    const uint8_t missing[] = {0x64, 0x65};
    const uint8_t *part = NULL;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t eak_len;
    size_t len;
    size_t total;
    size_t i;
    uint8_t sum = 0;

    base_config(&cfg);
    cfg.max_outgoing = 30;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 30, 1024, 1000, 100,
                             3, 1, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Ten data frames fill the application allowance, then an EAK fills the
     * two protocol-reserved slots with retransmissions. */
    for (i = 0; i < 10u; i++) {
        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1u, 0) ==
              CP_LINK_OK);
    }
    eak_len = make_frame(eak, sizeof(eak), 0x20, 0x63, 0x2a, 0, missing,
                         sizeof(missing), true);
    feed_frame(&g_link, eak, eak_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)CP_LINK_OUTPUT_FRAMES * 11u);

    /* The peer frame raises an ACK opportunity, but every output slot is
     * occupied by data. The ACK stays owed instead of resetting the link. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_generation(&g_link) == 1u);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)CP_LINK_OUTPUT_FRAMES * 11u);

    /* Draining one frame retries the owed ACK and appends it at the tail. */
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 11u);
    CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)(CP_LINK_OUTPUT_FRAMES - 1u) * 11u + CP_LINK_HEADER_LEN);

    total = drain_all(&g_link, out, sizeof(out));
    CHECK(total == (size_t)(CP_LINK_OUTPUT_FRAMES - 1u) * 11u +
                       CP_LINK_HEADER_LEN);
    CHECK(out[total - 9u + 4u] == 0x40u);
    CHECK(out[total - 9u + 6u] == 0x2bu);
    CHECK(out[total - 9u + 7u] == 0u);
    for (i = 0; i < CP_LINK_HEADER_LEN; i++)
        sum = (uint8_t)(sum + out[total - CP_LINK_HEADER_LEN + i]);
    CHECK(sum == 0u);
    puts("PASS: ACK owed on a full data queue is deferred and queued after drain");
}

static void test_window_full_backpressure(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t ack[CP_LINK_HEADER_LEN];
    uint8_t frames[256];
    const uint8_t sessions[] = {3, 0, 1, 5, 2, 1};
    const uint8_t payload = 0x44;
    size_t lsp_len;
    size_t peer_len;
    size_t ack_len;
    size_t frames_len;
    unsigned i;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 1024, 1000, 100, 3,
                             2, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    for (i = 0; i < 4u; i++) {
        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1, 0) ==
              CP_LINK_OK);
    }
    CHECK(cp_link_writable(&g_link) == false);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1, 0) ==
          CP_LINK_ERR_BACKPRESSURE);
    CHECK(cp_link_stats(&g_link)->tx_backpressure >= 1u);

    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x64, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 1);
    CHECK(cp_link_writable(&g_link));
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1, 1) ==
          CP_LINK_OK);
    frames_len = drain_all(&g_link, frames, sizeof(frames));
    CHECK(frames_len == 55u);
    CHECK(frames[5] == 0x64u);
    CHECK(frames[16] == 0x65u);
    CHECK(frames[27] == 0x66u);
    CHECK(frames[38] == 0x67u);
    CHECK(frames[49] == 0x68u);
    puts("PASS: peer receive window backpressure and ACK-driven resumption");
}

static void test_sequence_wrap_and_duplicates(void)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    size_t peer_len;
    size_t frame_len;
    const uint8_t a = 'A';
    const uint8_t b = 'B';
    const uint8_t c = 'C';
    const uint8_t d = 'D';
    const uint8_t x = 'X';

    base_config(&cfg);
    peer_len = hex2bin(
        "ff5a001d80fe00000c"
        "0108040003e800640302030001050201090102"
        "87",
        peer_syn, sizeof(peer_syn));
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0xFF, 0x63, 3, &a, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x00, 0x63, 3, &b, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    /* Exact duplicate of the wrapped sequence number. */
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_stats(&g_link)->duplicates >= 1u);
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Out of order: 0x02 arrives before 0x01. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x02, 0x63, 3, &d, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 2u);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x01, 0x63, 3, &c, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 4u);
    CHECK(g_events[0].data[0] == 'A');
    CHECK(g_events[1].data[0] == 'B');
    CHECK(g_events[2].data[0] == 'C');
    CHECK(g_events[3].data[0] == 'D');

    /* Old sequence number (more than 127 behind) must not be delivered. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0xFD, 0x63, 3, &x, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 4u);
    CHECK(cp_link_stats(&g_link)->out_of_window >= 1u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: modulo-256 sequence wrap, duplicate suppression and out-of-order delivery");
}

static void test_retransmit_identical_and_ack(void)
{
    cp_link_config_t cfg;
    uint8_t sent[CP_LINK_MAX_FRAME];
    uint8_t first[CP_LINK_MAX_FRAME];
    uint8_t second[CP_LINK_MAX_FRAME];
    uint8_t ack[CP_LINK_MAX_FRAME];
    uint8_t payload = 0xAA;
    size_t sent_len;
    size_t first_len;
    size_t second_len;
    size_t ack_len;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1, 0) ==
          CP_LINK_OK);
    sent_len = drain_all(&g_link, sent, sizeof(sent));
    CHECK(sent_len == 11u);
    CHECK(sent[5] == 0x64u);
    CHECK(sent[7] == 3u);
    CHECK(cp_link_next_deadline(&g_link) == 1000u);

    CHECK(cp_link_tick(&g_link, 999u) == CP_LINK_OK);
    expect_no_output(&g_link);
    /* No peer data has advanced the received sequence, so the refreshed
     * retransmission must remain byte-identical, including ACK/checksum. */
    CHECK(cp_link_tick(&g_link, 1000u) == CP_LINK_OK);
    first_len = drain_all(&g_link, first, sizeof(first));
    CHECK(first_len == sent_len);
    CHECK(memcmp(first, sent, sent_len) == 0);
    CHECK(cp_link_tick(&g_link, 2000u) == CP_LINK_OK);
    second_len = drain_all(&g_link, second, sizeof(second));
    CHECK(second_len == sent_len);
    CHECK(memcmp(second, sent, sent_len) == 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_tick(&g_link, 3000u) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_ACK_TIMEOUT);
    CHECK(cp_link_generation(&g_link) == 2u);
    expect_no_output(&g_link);
    CHECK(cp_link_stats(&g_link)->retransmits == 2u);

    /* An ACK removes the retransmit deadline entirely. */
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1, 0) ==
          CP_LINK_OK);
    {
        uint8_t discard[64];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x64, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 10);
    CHECK(cp_link_next_deadline(&g_link) == UINT64_MAX);
    CHECK(cp_link_tick(&g_link, 5000u) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    expect_no_output(&g_link);
    puts("PASS: identical retransmission bytes, lost ACK retry and finite exhaustion");
}

static void test_retransmit_refreshes_ack(void)
{
    cp_link_config_t cfg;
    uint8_t sent[CP_LINK_MAX_FRAME];
    uint8_t first[CP_LINK_MAX_FRAME];
    uint8_t second[CP_LINK_MAX_FRAME];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t eak[CP_LINK_MAX_FRAME];
    uint8_t payload1 = 0xAA;
    uint8_t payload2 = 0xBB;
    uint8_t peer_payload = 0x55;
    const uint8_t requested = 0x64u;
    size_t sent_len;
    size_t first_len;
    size_t second_len;
    size_t frame_len;
    size_t eak_len;
    size_t i;
    uint8_t sum;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload1, 1u, 0) ==
          CP_LINK_OK);
    sent_len = drain_all(&g_link, sent, sizeof(sent));
    CHECK(sent_len == 11u);
    CHECK(sent[5] == 0x64u);
    CHECK(sent[6] == 0x2au);
    CHECK(sent[7] == 3u);

    /* Advance the received sequence without acknowledging the data frame. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &peer_payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_tick(&g_link, 100u) == CP_LINK_OK);
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    CHECK(cp_link_tick(&g_link, 1000u) == CP_LINK_OK);
    first_len = drain_all(&g_link, first, sizeof(first));
    CHECK(first_len == sent_len);
    CHECK(memcmp(first, sent, 6u) == 0);
    CHECK(first[6] == 0x2bu);
    CHECK(first[7] == sent[7]);
    CHECK(memcmp(first + 9u, sent + 9u, sent_len - 9u) == 0);
    CHECK(first[8] != sent[8]);
    sum = 0;
    for (i = 0; i < CP_LINK_HEADER_LEN; i++)
        sum = (uint8_t)(sum + first[i]);
    CHECK(sum == 0u);

    /* With no new peer data the refreshed copy is byte-identical. */
    CHECK(cp_link_tick(&g_link, 2000u) == CP_LINK_OK);
    second_len = drain_all(&g_link, second, sizeof(second));
    CHECK(second_len == first_len);
    CHECK(memcmp(second, first, first_len) == 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);

    /* EAK-driven retransmission uses the same refreshed ACK path. */
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload2, 1u, 0) ==
          CP_LINK_OK);
    sent_len = drain_all(&g_link, sent, sizeof(sent));
    CHECK(sent_len == 11u);
    CHECK(sent[5] == 0x64u);

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &peer_payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    eak_len = make_frame(eak, sizeof(eak), 0x20, 0x63, 0x2b, 0,
                         &requested, 1u, true);
    feed_frame(&g_link, eak, eak_len, 0);
    first_len = drain_all(&g_link, first, sizeof(first));
    CHECK(first_len == sent_len);
    CHECK(memcmp(first, sent, 6u) == 0);
    CHECK(first[6] == 0x2bu);
    CHECK(first[7] == sent[7]);
    CHECK(memcmp(first + 9u, sent + 9u, sent_len - 9u) == 0);
    sum = 0;
    for (i = 0; i < CP_LINK_HEADER_LEN; i++)
        sum = (uint8_t)(sum + first[i]);
    CHECK(sum == 0u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: DATA retransmission keeps seq/session/payload and refreshes ACK");
}

static void test_retransmit_partial_queue_is_immutable(void)
{
    cp_link_config_t cfg;
    uint8_t first[CP_LINK_MAX_FRAME];
    uint8_t remaining[CP_LINK_MAX_FRAME];
    uint8_t payload = 0xCC;
    const uint8_t *part = NULL;
    size_t len;
    size_t first_len;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1u, 0) ==
          CP_LINK_OK);
    {
        uint8_t discard[64];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    CHECK(cp_link_tick(&g_link, 1000u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 11u && part != NULL);
    memcpy(first, part, len);
    first_len = len;
    CHECK(cp_link_output_consume(&g_link, 3u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 8u);
    memcpy(remaining, part, len);

    CHECK(cp_link_tick(&g_link, 2000u) == CP_LINK_OK);
    CHECK(cp_link_output_pending(&g_link) == 8u + 11u);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 8u && memcmp(part, remaining, len) == 0);
    CHECK(cp_link_output_consume(&g_link, 8u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == first_len && memcmp(part, first, len) == 0);
    CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
    expect_no_output(&g_link);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: partially emitted retransmission is immutable; retry appends");
}

static void test_eak_and_out_of_window(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_MAX_FRAME];
    const uint8_t sessions[] = {3, 0, 1, 5, 2, 1, 9, 1, 2};
    const uint8_t payload1 = 0x10;
    const uint8_t payload2 = 0x11;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    size_t second_len;
    uint8_t missing[7];
    size_t i;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 1024, 1000, 100, 3,
                             2, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload1, 1, 0) ==
          CP_LINK_OK);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload2, 1, 0) ==
          CP_LINK_OK);
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    /* Recover the exact second frame from the retransmission output. */
    frame_len = make_frame(expected, sizeof(expected), 0x40, 0x65, 0x2a, 3,
                           &payload2, 1, true);
    second_len = frame_len;

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0, 0x64, 0, NULL, 0,
                           false);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_next_deadline(&g_link) == 1000u);

    {
        uint8_t one_missing = 0x65u;

        frame_len = make_frame(frame, sizeof(frame), 0x20, 0x63, 0x2a, 0,
                               &one_missing, 1, true);
    }
    feed_frame(&g_link, frame, frame_len, 0);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));
        CHECK(got_len == second_len);
        CHECK(memcmp(got, expected, second_len) == 0);
    }
    CHECK(cp_link_next_deadline(&g_link) == 1000u);

    feed_frame(&g_link, frame, frame_len, 10);
    {
        uint8_t discard[64];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    feed_frame(&g_link, frame, frame_len, 20);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_ACK_TIMEOUT);
    CHECK(cp_link_generation(&g_link) == 2u);
    expect_no_output(&g_link);

    /* Out-of-window data and a gap that triggers EAK. */
    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x63, 0x63, 3,
                           &payload1, 1, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    CHECK(cp_link_stats(&g_link)->out_of_window >= 1u);
    expected_len = make_frame(expected, sizeof(expected), 0x40, 0x63, 0x2a, 0,
                              NULL, 0, false);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));
        CHECK(got_len == expected_len);
        CHECK(memcmp(got, expected, expected_len) == 0);
    }

    for (i = 0; i < sizeof(missing); i++)
        missing[i] = (uint8_t)(0x2Bu + i);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload2, 1, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a, 0,
                              missing, sizeof(missing), true);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));
        CHECK(got_len == expected_len);
        CHECK(memcmp(got, expected, expected_len) == 0);
    }
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);

    /* Fill the hole; buffered 0x32 must be delivered exactly once. */
    {
        uint8_t stream[512];
        size_t off = 0;

        for (i = 0; i < sizeof(missing); i++) {
            uint8_t value = (uint8_t)i;
            off += make_frame(stream + off, sizeof(stream) - off, 0x40,
                              missing[i], 0x63, 3, &value, 1u, true);
        }
        feed_bytes(&g_link, stream, off, 0);
    }
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == sizeof(missing) + 1u);
    CHECK(g_events[sizeof(missing)].data[0] == payload2);
    puts("PASS: out-of-window ACK, gap EAK and EAK retransmission exhaustion");
}

static void test_eak_recomputes_missing_list(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_MAX_FRAME];
    uint8_t missing[7];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x33;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 1024, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* A below-threshold reordered packet is buffered without an EAK. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2d, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    expect_no_output(&g_link);

    /* A later far frame opens the gap; the buffered packet is not requested. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2bu;
    missing[1] = 0x2cu;
    missing[2] = 0x2eu;
    missing[3] = 0x2fu;
    missing[4] = 0x30u;
    missing[5] = 0x31u;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 6u, true);
    expect_peek_bytes(&g_link, expected, expected_len,
                      "initial EAK excludes an already buffered packet");
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);

    /* A new frontier keeps the earlier buffered packet excluded. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x34, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[6] = 0x33u;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 7u, true);
    expect_peek_bytes(&g_link, expected, expected_len,
                      "EAK advances the frontier without re-requesting data");

    /* Receiving the frontier-adjacent packet removes it from the request. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x33, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 6u, true);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "EAK coalesces into one updated frame");
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    puts("PASS: EAK recomputes gaps from reorder slots and coalesces updates");
}

static void test_eak_stale_gap_fill_retracts_frame(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t stream[512];
    uint8_t out[CP_LINK_MAX_FRAME * 2u];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x44;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t stream_len = 0;
    size_t total;
    size_t i;
    capture_event_t *event;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 1024, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Queue an EAK but do not let the caller drain it. Append an ACK behind it
     * so the EAK is not the tail when the gap closes. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_output_pending(&g_link) == 17u);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2a, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_output_pending(&g_link) == 17u + CP_LINK_HEADER_LEN);

    /* Close every hole before the EAK is emitted. The queued EAK must be
     * retracted and the buffered 0x32 delivered exactly once. */
    for (i = 0; i < 7u; i++) {
        uint8_t value = (uint8_t)i;

        stream_len += make_frame(stream + stream_len,
                                 sizeof(stream) - stream_len, 0x40,
                                 (uint8_t)(0x2bu + i), 0x63, 3, &value, 1u,
                                 true);
    }
    feed_bytes(&g_link, stream, stream_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 8u);
    event = last_event(CP_LINK_EVENT_SESSION_DATA);
    CHECK(event != NULL && event->len == 1u && event->data[0] == payload);

    total = drain_all(&g_link, out, sizeof(out));
    CHECK(!drained_has_control(out, total, 0x20, NULL));
    puts("PASS: EAK is retracted when the gap closes before output drain");
}

static void test_eak_full_output_is_deferred(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t inbound_eak[CP_LINK_MAX_FRAME];
    uint8_t out[512];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x55;
    const uint8_t far_payload = 0x11;
    const uint8_t requested[] = {0x64, 0x65};
    const uint8_t *part = NULL;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t eak_len;
    size_t len;
    size_t total;
    size_t i;
    uint8_t sum = 0;

    base_config(&cfg);
    cfg.max_outgoing = 30;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 30, 1024, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Fill the output with ten application frames plus two retransmissions. */
    for (i = 0; i < 10u; i++) {
        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1u, 0) ==
              CP_LINK_OK);
    }
    eak_len = make_frame(inbound_eak, sizeof(inbound_eak), 0x20, 0x63, 0x2a,
                         0, requested, sizeof(requested), true);
    feed_frame(&g_link, inbound_eak, eak_len, 0);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)CP_LINK_OUTPUT_FRAMES * 11u);

    /* A valid out-of-order burst crosses the EAK threshold while output is
     * full. The EAK stays pending; the link stays healthy and no data is
     * dropped. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x49, 0x63, 3,
                           &far_payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x4b, 0x63, 3,
                           &far_payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_generation(&g_link) == 1u);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    CHECK(cp_link_stats(&g_link)->out_of_window == 0u);
    CHECK(cp_link_stats(&g_link)->reorder_full == 0u);
    CHECK(cp_link_stats(&g_link)->eak_sent == 0u);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)CP_LINK_OUTPUT_FRAMES * 11u);
    CHECK(cp_link_tick(&g_link, 0u) == CP_LINK_OK);
    CHECK(g_link.eak_due);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)CP_LINK_OUTPUT_FRAMES * 11u);

    /* Draining one frame flushes the deferred EAK. */
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 11u);
    CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)(CP_LINK_OUTPUT_FRAMES - 1u) * 11u + 41u);

    /* A new buffered packet rewrites the one unconsumed EAK in place. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x4a, 0x63, 3,
                           &far_payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)(CP_LINK_OUTPUT_FRAMES - 1u) * 11u + 40u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);

    total = drain_all(&g_link, out, sizeof(out));
    CHECK(total == (size_t)(CP_LINK_OUTPUT_FRAMES - 1u) * 11u + 40u);
    for (i = 0; i < (size_t)(CP_LINK_OUTPUT_FRAMES - 1u); i++) {
        size_t offset = i * 11u;

        CHECK(((size_t)out[offset + 2u] << 8 | out[offset + 3u]) == 11u);
        CHECK(out[offset + 4u] == 0x40u);
    }
    CHECK(out[total - 40u + 4u] == 0x20u);
    CHECK(out[total - 40u + 6u] == 0x2au);
    for (i = 0; i < 30u; i++)
        CHECK(out[total - 40u + 9u + i] == (uint8_t)(0x2bu + i));
    for (i = 0; i < CP_LINK_HEADER_LEN; i++)
        sum = (uint8_t)(sum + out[total - 40u + i]);
    CHECK(sum == 0u);
    puts("PASS: full output defers EAK, then flushes and coalesces safely");
}

static void test_eak_partial_output_is_immutable(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_MAX_FRAME];
    uint8_t remaining[CP_LINK_MAX_FRAME];
    uint8_t missing[8];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x66;
    const uint8_t *part = NULL;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    size_t len;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 1024, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_output_consume(&g_link, 4u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 13u && part != NULL);
    memcpy(remaining, part, len);

    /* A new threshold crossing may append a fresh EAK, but the emitted prefix
     * and remaining bytes of the first EAK are never rewritten. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x34, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_output_pending(&g_link) == 13u + 18u);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 13u && memcmp(part, remaining, len) == 0);
    CHECK(cp_link_output_consume(&g_link, 13u) == CP_LINK_OK);

    fill_sequence(missing, 8u, 0x2bu);
    missing[7] = 0x33u;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 8u, true);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == expected_len);
    CHECK(memcmp(part, expected, expected_len) == 0);
    CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
    expect_no_output(&g_link);
    CHECK(cp_link_stats(&g_link)->eak_sent == 2u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: partially emitted EAK is immutable; a fresh EAK follows it");
}

static void test_eak_partial_truncated_followup(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_MAX_FRAME];
    uint8_t remaining[CP_LINK_MAX_FRAME];
    uint8_t missing[2];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x71;
    const uint8_t *part = NULL;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    size_t len;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 12, 1000, 100, 3,
                             8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2bu;
    missing[1] = 0x2cu;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 2u, true);
    CHECK(expected_len == 12u);
    CHECK(g_link.eak_more);
    CHECK(cp_link_output_consume(&g_link, 4u) == CP_LINK_OK);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 8u && part != NULL);
    memcpy(remaining, part, len);

    /* The partial EAK is immutable, but filling the first requested hole
     * appends a fresh truncated EAK for the remaining holes. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_output_pending(&g_link) == 8u + 12u);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == 8u && memcmp(part, remaining, len) == 0);
    CHECK(cp_link_output_consume(&g_link, 8u) == CP_LINK_OK);

    missing[0] = 0x2cu;
    missing[1] = 0x2du;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2b,
                              0, missing, 2u, true);
    CHECK(expected_len == 12u);
    len = cp_link_output_peek(&g_link, &part);
    CHECK(len == expected_len);
    CHECK(memcmp(part, expected, expected_len) == 0);
    CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 2u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);
    puts("PASS: partial truncated EAK stays immutable; remaining holes follow up");
}

static void test_eak_modulo_and_pending_clear(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_MAX_FRAME];
    uint8_t inbound_eak[CP_LINK_MAX_FRAME];
    uint8_t out[CP_LINK_MAX_FRAME * 2u];
    uint8_t missing[3];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x77;
    const uint8_t requested[] = {0x64, 0x65};
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    size_t eak_len;
    size_t total;
    size_t i;

    base_config(&cfg);
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 1024, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0xFE, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x02, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0xFFu;
    missing[1] = 0x00u;
    missing[2] = 0x01u;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0xFE,
                              0, missing, sizeof(missing), true);
    expect_peek_bytes(&g_link, expected, expected_len,
                      "EAK wraps across the sequence boundary");

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x00, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0xFFu;
    missing[1] = 0x01u;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0xFE,
                              0, missing, 2u, true);
    expect_peek_bytes(&g_link, expected, expected_len,
                      "EAK removes a wrapped packet already buffered");

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0xFF, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x01u;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x00,
                              0, missing, 1u, true);
    expect_peek_bytes(&g_link, expected, expected_len,
                      "EAK advances ACK and frontier across the wrap");

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x01, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    total = drain_all(&g_link, out, sizeof(out));
    CHECK(!drained_has_control(out, total, 0x20, NULL));

    /* Re-negotiate a wider window, fill the output, defer an EAK, then cancel:
     * pending state must clear. */
    cfg.max_outgoing = 30;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 30, 1024, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    for (i = 0; i < 10u; i++) {
        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1u, 0) ==
              CP_LINK_OK);
    }
    eak_len = make_frame(inbound_eak, sizeof(inbound_eak), 0x20, 0x63, 0x2a,
                         0, requested, sizeof(requested), true);
    feed_frame(&g_link, inbound_eak, eak_len, 0);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x49, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(g_link.eak_due);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);

    CHECK(cp_link_cancel(&g_link, CP_LINK_RESET_CANCELLED) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(!g_link.eak_due);
    expect_no_output(&g_link);
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 100u) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DETECT);
    CHECK(!g_link.eak_due);
    expect_wire(&g_link, "ff550200ee10", "restart marker after cancelled EAK");
    puts("PASS: modulo-256 EAK gaps close cleanly and cancellation clears pending EAK");
}

static void test_eak_peer_max_len_capacity(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t expected[CP_LINK_MAX_FRAME];
    uint8_t stream[256];
    uint8_t out[CP_LINK_MAX_FRAME * 2u];
    uint8_t missing[13];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x21;
    const uint16_t tiny_len[] = {9u, 10u};
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t expected_len;
    size_t total;
    size_t i;
    unsigned j;

    base_config(&cfg);

    /* A peer that cannot carry even a one-byte EAK payload gets an ordinary
     * cumulative ACK (nine-byte header only), not an oversized EAK. */
    for (j = 0; j < 2u; j++) {
        lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, tiny_len[j],
                                 1000, 100, 3, 8, sessions,
                                 sizeof(sessions));
        peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                              peer_lsp, lsp_len, true);
        CHECK(peer_len > tiny_len[j]);
        handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
        clear_events();
        {
            uint8_t discard[128];
            (void)drain_all(&g_link, discard, sizeof(discard));
        }

        frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                               &payload, 1u, true);
        feed_frame(&g_link, frame, frame_len, 0);
        expected_len = make_frame(expected, sizeof(expected), 0x40, 0x63,
                                  0x2a, 0, NULL, 0, false);
        CHECK(expected_len == 9u);
        CHECK(expected_len <= tiny_len[j]);
        CHECK(cp_link_output_pending(&g_link) == expected_len);
        expect_wire_bytes(&g_link, expected, expected_len,
                          "EAK fallback is a cumulative ACK");
        CHECK(cp_link_stats(&g_link)->eak_sent == 0u);
        CHECK(cp_link_stats(&g_link)->reset_count == 0u);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
        CHECK(cp_link_tick(&g_link, 1000u) == CP_LINK_OK);
        expect_no_output(&g_link);
    }

    /* One-byte EAK payload capacity (max_len 11). Consume the truncated EAK,
     * then close requested holes; each progress step must request the next
     * remaining hole without a new gap threshold. */
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 11, 1000, 100, 3,
                             8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2bu;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 1u, true);
    CHECK(expected_len == 11u);
    CHECK(cp_link_output_pending(&g_link) == expected_len);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "one-byte EAK fits max_len 11");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);
    CHECK(cp_link_output_pending(&g_link) == 0u);
    CHECK(cp_link_tick(&g_link, 0u) == CP_LINK_OK);
    CHECK(cp_link_tick(&g_link, 1000u) == CP_LINK_OK);
    expect_no_output(&g_link);

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2cu;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2b,
                              0, missing, 1u, true);
    CHECK(expected_len == 11u);
    CHECK(cp_link_output_pending(&g_link) == expected_len);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "consumed EAK follows up after first hole fills");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 2u);

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2du;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2c,
                              0, missing, 1u, true);
    CHECK(expected_len == 11u);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "next consumed EAK follows up again");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 3u);

    /* Closing every remaining hole clears the truncated-followup state. */
    frame_len = 0;
    for (i = 0; i < 5u; i++) {
        uint8_t value = (uint8_t)i;

        frame_len += make_frame(stream + frame_len,
                                sizeof(stream) - frame_len, 0x40,
                                (uint8_t)(0x2du + i), 0x63, 3, &value, 1u,
                                true);
    }
    feed_bytes(&g_link, stream, frame_len, 0);
    CHECK(!g_link.eak_more);
    total = drain_all(&g_link, out, sizeof(out));
    CHECK(!drained_has_control(out, total, 0x20, NULL));
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);

    /* Two-byte EAK payload capacity (max_len 12). */
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 12, 1000, 100, 3,
                             8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x32, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2bu;
    missing[1] = 0x2cu;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 2u, true);
    CHECK(expected_len == 12u);
    CHECK(cp_link_output_pending(&g_link) == expected_len);
    expect_peek_bytes(&g_link, expected, expected_len,
                      "two-byte EAK fits max_len 12");

    /* The queued EAK is rewritten in place at the same ceiling as the first
     * requested hole closes. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2cu;
    missing[1] = 0x2du;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2b,
                              0, missing, 2u, true);
    CHECK(expected_len == 12u);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "clamped EAK rewrite keeps max_len 12");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);
    CHECK(cp_link_tick(&g_link, 0u) == CP_LINK_OK);
    expect_no_output(&g_link);

    /* After consumption, the next requested hole advances the followup. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    missing[0] = 0x2du;
    missing[1] = 0x2eu;
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2c,
                              0, missing, 2u, true);
    CHECK(expected_len == 12u);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "consumed max_len 12 EAK follows up");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 2u);

    /* A larger bounded list is truncated to max_len - header - checksum. */
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 4, 20, 1000, 100, 3,
                             8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x38, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    fill_sequence(missing, 10u, 0x2bu);
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2a,
                              0, missing, 10u, true);
    CHECK(expected_len == 20u);
    CHECK(cp_link_output_pending(&g_link) == expected_len);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "larger EAK list is clamped to peer max_len");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 1u);
    CHECK(cp_link_output_pending(&g_link) == 0u);
    CHECK(cp_link_tick(&g_link, 0u) == CP_LINK_OK);
    expect_no_output(&g_link);

    /* After the truncated EAK is consumed, the first requested hole advances
     * the list beyond the previously requested prefix. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    fill_sequence(missing, 10u, 0x2cu);
    expected_len = make_frame(expected, sizeof(expected), 0x20, 0x63, 0x2b,
                              0, missing, 10u, true);
    CHECK(expected_len == 20u);
    expect_wire_bytes(&g_link, expected, expected_len,
                      "consumed max_len 20 EAK follows up after a hole fills");
    CHECK(g_link.eak_more);
    CHECK(cp_link_stats(&g_link)->eak_sent == 2u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);

    /* Cancellation clears truncated-followup state; a new generation starts
     * clean. */
    CHECK(cp_link_cancel(&g_link, CP_LINK_RESET_CANCELLED) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(!g_link.eak_more);
    CHECK(!g_link.eak_due);
    CHECK(g_link.eak_last_count == 0u);
    expect_no_output(&g_link);
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 100u) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DETECT);
    CHECK(!g_link.eak_more);
    expect_wire(&g_link, "ff550200ee10", "clean restart after truncated EAK");
    puts("PASS: EAK respects peer max_len, clamps lists and falls back to ACK");
}

static void test_eak_ring_wrap_nonhead_compaction(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t stream[512];
    uint8_t out[CP_LINK_MAX_FRAME * 2u];
    const uint8_t sessions[] = {3, 0, 1};
    const uint8_t payload = 0x23;
    const uint8_t far_payload = 0x24;
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t stream_len;
    size_t total;
    size_t offset;
    size_t i;
    unsigned acks = 0;

    base_config(&cfg);
    cfg.max_outgoing = 30;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 30, 12, 1000, 100,
                             3, 8, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Move the ring head to index 10 by draining 19 one-byte data frames. */
    for (i = 0; i < 19u; i++) {
        CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1u, 0) ==
              CP_LINK_OK);
        if ((i + 1u) % 10u == 0u) {
            uint8_t discard[256];
            (void)drain_all(&g_link, discard, sizeof(discard));
        }
    }
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_output_pending(&g_link) == 0u);
    CHECK(g_link.out_head == 10u);
    CHECK(g_link.out_count == 0u);

    /* Queue ACK(index 10), EAK(index 11), ACK(index 0): a wrapped ring with
     * the EAK in the middle. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2a, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x49, 0x63, 3,
                           &far_payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2a, 0x63, 3,
                           &payload, 1u, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(g_link.eak_more);
    CHECK(cp_link_output_pending(&g_link) ==
          CP_LINK_HEADER_LEN + 12u + CP_LINK_HEADER_LEN);

    /* Closing every hole retracts the middle EAK and compacts the wrapped
     * trailing ACK into its place. */
    stream_len = 0;
    for (i = 0; i < 30u; i++) {
        uint8_t value = (uint8_t)i;

        stream_len += make_frame(stream + stream_len,
                                 sizeof(stream) - stream_len, 0x40,
                                 (uint8_t)(0x2bu + i), 0x63, 3, &value, 1u,
                                 true);
    }
    feed_bytes(&g_link, stream, stream_len, 0);
    CHECK(!g_link.eak_more);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->reset_count == 0u);

    total = drain_all(&g_link, out, sizeof(out));
    offset = 0;
    while (offset < total) {
        size_t length;
        uint8_t sum = 0;

        CHECK(total - offset >= CP_LINK_HEADER_LEN);
        length = ((size_t)out[offset + 2u] << 8) | out[offset + 3u];
        CHECK(length == CP_LINK_HEADER_LEN);
        CHECK(out[offset + 4u] == 0x40u);
        for (i = 0; i < CP_LINK_HEADER_LEN; i++)
            sum = (uint8_t)(sum + out[offset + i]);
        CHECK(sum == 0u);
        offset += length;
        acks++;
    }
    CHECK(offset == total);
    CHECK(acks >= 2u);
    puts("PASS: wrapped ring compacts a non-head EAK without corrupting output");
}

static void test_eak_output_burst_is_deferred(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t eak[CP_LINK_MAX_FRAME];
    uint8_t missing[30];
    const uint8_t sessions[] = {3, 0, 1, 5, 2, 1, 9, 1, 2};
    const uint8_t payload = 0x55;
    size_t lsp_len;
    size_t peer_len;
    size_t eak_len;
    unsigned batch;
    unsigned i;
    unsigned guard = 0;

    base_config(&cfg);
    cfg.max_outgoing = 30;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 30, 1024, 1000, 100,
                             3, 2, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* Fill 30 tx slots, draining output between batches. */
    for (batch = 0; batch < 3u; batch++) {
        for (i = 0; i < 10u; i++) {
            CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &payload, 1,
                               0) == CP_LINK_OK);
        }
        {
            uint8_t discard[256];
            (void)drain_all(&g_link, discard, sizeof(discard));
        }
    }
    for (i = 0; i < sizeof(missing); i++)
        missing[i] = (uint8_t)(0x64u + i);

    eak_len = make_frame(eak, sizeof(eak), 0x20, 0x63, 0x2a, 0, missing,
                         sizeof(missing), true);
    feed_frame(&g_link, eak, eak_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_stats(&g_link)->retransmits == CP_LINK_OUTPUT_FRAMES);
    CHECK(cp_link_output_pending(&g_link) ==
          (size_t)CP_LINK_OUTPUT_FRAMES * 11u);
    CHECK(cp_link_writable(&g_link) == false);
    CHECK(cp_link_next_deadline(&g_link) == UINT64_MAX);

    /* Drain one frame per pass: deferred retransmissions resume, never reset. */
    while (cp_link_stats(&g_link)->retransmits < sizeof(missing) &&
           guard++ < 100u) {
        const uint8_t *part = NULL;
        size_t len = cp_link_output_peek(&g_link, &part);

        CHECK(len != 0);
        CHECK(cp_link_output_consume(&g_link, len) == CP_LINK_OK);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
        CHECK(cp_link_tick(&g_link, 0) == CP_LINK_OK);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    }
    CHECK(guard < 100u);
    CHECK(cp_link_stats(&g_link)->retransmits == sizeof(missing));
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    puts("PASS: EAK retransmission burst defers on output backpressure without reset");
}

static void test_reset_cancel_and_generation(void)
{
    cp_link_config_t cfg;
    uint8_t frame[CP_LINK_MAX_FRAME];
    size_t frame_len;
    capture_event_t *event;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x10, 0, 0x2a, 0, NULL, 0,
                           false);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_PEER);
    CHECK(cp_link_generation(&g_link) == 2u);
    CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_CONTROL) ==
          CP_LINK_SESSION_ID_NONE);
    CHECK(cp_link_peer_sync(&g_link) == NULL);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, "x", 1, 0) ==
          CP_LINK_ERR_STATE);
    expect_no_output(&g_link);
    event = last_event(CP_LINK_EVENT_RESET);
    CHECK(event != NULL);
    CHECK(event->reason == CP_LINK_RESET_PEER);
    CHECK(event->generation == 2u);

    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 100) == CP_LINK_OK);
    CHECK(cp_link_generation(&g_link) == 3u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DETECT);
    expect_wire(&g_link, "ff550200ee10", "restart marker");

    /* Local cancellation clears queued output and the negotiated identity. */
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_cancel(&g_link, CP_LINK_RESET_CANCELLED) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_CANCELLED);
    CHECK(cp_link_generation(&g_link) == 2u);
    CHECK(cp_link_session_id(&g_link, CP_LINK_SESSION_CONTROL) ==
          CP_LINK_SESSION_ID_NONE);
    expect_no_output(&g_link);

    /* Local abort leaves exactly one explicit RST frame. */
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_abort(&g_link, 50, CP_LINK_RESET_LOCAL_ABORT) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_LOCAL_ABORT);
    CHECK(cp_link_generation(&g_link) == 2u);
    expect_wire(&g_link, "ff5a000910632a0001", "local abort RST");
    puts("PASS: peer RST, cancellation and abort advance the auth generation");
}

static void test_handshake_bounds_and_marker(void)
{
    cp_link_config_t cfg;
    static const uint8_t wrong_marker[6] = {0, 0, 0, 0, 0, 0};

    base_config(&cfg);
    cfg.detect_retry_limit = 2;
    cfg.detect_interval_ms = 1000;
    cfg.handshake_timeout_ms = 10000;

    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    clear_events();
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 0) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker");
    CHECK(cp_link_tick(&g_link, 1000) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker retry 1");
    CHECK(cp_link_tick(&g_link, 2000) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker retry 2");
    CHECK(cp_link_tick(&g_link, 3000) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_DETECT_EXHAUSTED);
    CHECK(cp_link_generation(&g_link) == 2u);
    expect_no_output(&g_link);

    /* SYN retries rebuild deterministically while nothing has been received:
     * same sequence, payload, ACK and checksum. */
    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 0) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker");
    feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
    {
        uint8_t local_syn[CP_LINK_MAX_FRAME];
        uint8_t first[CP_LINK_MAX_FRAME];
        size_t local_len;
        size_t first_len;

        expected_local_syn(&cfg, local_syn, &local_len);
        {
            uint8_t got[CP_LINK_MAX_FRAME];
            size_t got_len = drain_all(&g_link, got, sizeof(got));

            CHECK(got_len == local_len);
            CHECK(memcmp(got, local_syn, local_len) == 0);
        }
        CHECK(cp_link_tick(&g_link, 500) == CP_LINK_OK);
        first_len = drain_all(&g_link, first, sizeof(first));
        CHECK(first_len == local_len);
        CHECK(memcmp(first, local_syn, local_len) == 0);
        CHECK(cp_link_tick(&g_link, 1000) == CP_LINK_OK);
        first_len = drain_all(&g_link, first, sizeof(first));
        CHECK(first_len == local_len);
        CHECK(memcmp(first, local_syn, local_len) == 0);
        CHECK(cp_link_tick(&g_link, 1500) == CP_LINK_OK);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
        CHECK(cp_link_reset_reason(&g_link) ==
              CP_LINK_RESET_NEGOTIATE_EXHAUSTED);
        expect_no_output(&g_link);
    }

    /* Absolute handshake bound fires even when retry limits are high. */
    cfg.detect_retry_limit = 20;
    cfg.handshake_timeout_ms = 1200;
    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 0) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker");
    CHECK(cp_link_tick(&g_link, 1000) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker before deadline");
    CHECK(cp_link_tick(&g_link, 1200) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_HANDSHAKE_TIMEOUT);

    /* Passive marker mismatch is explicit and does not scan for a header. */
    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_PASSIVE, 0) == CP_LINK_OK);
    expect_wire(&g_link, "ff550200ee10", "marker");
    feed_bytes(&g_link, wrong_marker, sizeof(wrong_marker), 0);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) == CP_LINK_RESET_MARKER);
    puts("PASS: bounded marker/SYN retries, absolute handshake deadline and marker mismatch");
}

static void test_syn_resend_rebuilds_ack(void)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    uint8_t local_syn[CP_LINK_MAX_FRAME];
    size_t peer_len;
    size_t local_len;

    base_config(&cfg);
    peer_len = hex2bin(PEER_SYN_2A, peer_syn, sizeof(peer_syn));

    init_start_passive(&g_link, &cfg);
    feed_bytes(&g_link, (const uint8_t *)"\xff\x55\x02\x00\xee\x10", 6, 0);
    expected_local_syn(&cfg, local_syn, &local_len);
    {
        uint8_t got[CP_LINK_MAX_FRAME];
        size_t got_len = drain_all(&g_link, got, sizeof(got));

        CHECK(got_len == local_len);
        CHECK(memcmp(got, local_syn, local_len) == 0);
    }

    feed_frame(&g_link, peer_syn, peer_len, 0);
    expect_wire(&g_link, "ff5a000940632a00d1", "ACK for peer SYN");
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NEGOTIATE);
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* A retry after receiving the peer SYN keeps the original SYN sequence
     * and payload but carries the current cumulative ACK and checksum. */
    CHECK(cp_link_tick(&g_link, 500u) == CP_LINK_OK);
    expect_wire(&g_link,
                "ff5a001d80632a007d"
                "0104004a03e8006403020a00010b02010c0102"
                "35",
                "SYN retry with received ACK");
    CHECK(cp_link_tick(&g_link, 1000u) == CP_LINK_OK);
    expect_wire(&g_link,
                "ff5a001d80632a007d"
                "0104004a03e8006403020a00010b02010c0102"
                "35",
                "second SYN retry with received ACK");
    CHECK(cp_link_tick(&g_link, 1500u) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_DEAD);
    CHECK(cp_link_reset_reason(&g_link) ==
          CP_LINK_RESET_NEGOTIATE_EXHAUSTED);
    CHECK(cp_link_generation(&g_link) == 2u);
    expect_no_output(&g_link);
    puts("PASS: SYN resend rebuilds same sequence with current ACK/checksum");
}

static void test_zero_ack_mode(void)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t data = 0x22;
    size_t peer_len;
    size_t frame_len;
    unsigned accepted = 0;
    unsigned i;

    base_config(&cfg);
    peer_len = hex2bin(
        "ff5a001a802a0000e3"
        "01080400000000000000030001050201"
        "e7",
        peer_syn, sizeof(peer_syn));
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    for (i = 0; i < 20u; i++) {
        int rc = cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &data, 1, 0);

        if (rc == CP_LINK_OK)
            accepted++;
        else
            CHECK(rc == CP_LINK_ERR_BACKPRESSURE);
    }
    CHECK(accepted == 10u);
    CHECK(cp_link_next_deadline(&g_link) == UINT64_MAX);
    CHECK(cp_link_output_pending(&g_link) == accepted * 11u);
    {
        uint8_t discard[256];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x6D, 3, &data, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    expect_no_output(&g_link); /* max_ack == 0: no data ACK is generated */
    CHECK(cp_link_tick(&g_link, 60000) == CP_LINK_OK);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    expect_no_output(&g_link);
    puts("PASS: negotiated ZeroACK mode disables data ACKs and retransmit deadlines");
}

static void test_payload_bounds(void)
{
    cp_link_config_t cfg;
    uint8_t peer_lsp[64];
    uint8_t peer_syn[128];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t payload[64];
    uint8_t value = 'A';
    const uint8_t sessions[] = {3, 0, 1, 5, 2, 1};
    size_t lsp_len;
    size_t peer_len;
    size_t frame_len;
    size_t i;
    capture_event_t *event;

    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)i;
    base_config(&cfg);
    cfg.max_payload = 32;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 8, 42, 1000, 100, 3,
                             2, sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_max_payload(&g_link) == 32u);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, payload, 33, 0) ==
          CP_LINK_ERR_TOO_LARGE);
    expect_no_output(&g_link);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, payload, 32, 0) ==
          CP_LINK_OK);
    {
        size_t got_len;
        uint8_t got[CP_LINK_MAX_FRAME];

        got_len = drain_all(&g_link, got, sizeof(got));
        CHECK(got_len == 42u);
        CHECK(got[7] == 3u);
    }

    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2b, 0x64, 3, payload,
                           32, true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    event = last_event(CP_LINK_EVENT_SESSION_DATA);
    CHECK(event != NULL && event->len == 32u);

    /* Local receive bound is 42 bytes; a 43-byte header is dropped. */
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x64, 3, NULL, 0,
                           false);
    frame[2] = 0x00u;
    frame[3] = 43u;
    {
        uint8_t sum = 0;
        for (i = 0; i < 8u; i++)
            sum = (uint8_t)(sum + frame[i]);
        frame[8] = (uint8_t)((256u - (unsigned)sum) & 0xFFu);
    }
    feed_frame(&g_link, frame, 9u, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
    CHECK(cp_link_stats(&g_link)->frames_bad_length >= 1u);
    frame_len = make_frame(frame, sizeof(frame), 0x40, 0x2c, 0x64, 3, &value, 1,
                           true);
    feed_frame(&g_link, frame, frame_len, 0);
    CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 2u);

    /* Peer max_len 20 leaves exactly 10 bytes of payload. */
    base_config(&cfg);
    cfg.max_payload = 32;
    lsp_len = build_peer_lsp(peer_lsp, sizeof(peer_lsp), 8, 20, 1000, 100, 3, 2,
                             sessions, sizeof(sessions));
    peer_len = make_frame(peer_syn, sizeof(peer_syn), 0x80, 0x2a, 0, 0,
                          peer_lsp, lsp_len, true);
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_max_payload(&g_link) == 10u);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, payload, 11, 0) ==
          CP_LINK_ERR_TOO_LARGE);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, payload, 10, 0) ==
          CP_LINK_OK);
    puts("PASS: configured payload ceiling, peer max_len and local receive bound");
}

static void test_outbound_ack_wrap(void)
{
    cp_link_config_t cfg;
    uint8_t peer_syn[128];
    uint8_t frames[256];
    uint8_t ack[CP_LINK_MAX_FRAME];
    const uint8_t values[3] = {'A', 'B', 'C'};
    size_t peer_len;
    size_t frames_len;
    size_t ack_len;
    size_t off = 0;

    base_config(&cfg);
    cfg.initial_seq = 0xFE;
    peer_len = hex2bin(PEER_SYN_2A, peer_syn, sizeof(peer_syn));
    handshake_with_peer_lsp(&g_link, &cfg, peer_syn, peer_len);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &values[0], 1, 0) ==
          CP_LINK_OK);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &values[1], 1, 0) ==
          CP_LINK_OK);
    CHECK(cp_link_send(&g_link, CP_LINK_SESSION_CONTROL, &values[2], 1, 0) ==
          CP_LINK_OK);
    frames_len = drain_all(&g_link, frames, sizeof(frames));
    CHECK(frames_len == 33u);
    CHECK(frames[5] == 0xFFu);
    CHECK(frames[16] == 0x00u);
    CHECK(frames[27] == 0x01u);

    /* ACK 0x00 acknowledges both 0xFF and 0x00 across the wrap. */
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x00, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 1);
    CHECK(cp_link_next_deadline(&g_link) == 1000u);
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x01, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 2);
    CHECK(cp_link_next_deadline(&g_link) == UINT64_MAX);

    /* Stale, duplicate and unsent acknowledgements are ignored. */
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0xFE, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 3);
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x01, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 4);
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x02, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 5);
    CHECK(cp_link_stats(&g_link)->ack_ignored >= 3u);
    CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_link_next_deadline(&g_link) == UINT64_MAX);
    CHECK(cp_link_sequence_distance(0x00, 0xFF) == 1u);
    CHECK(cp_link_sequence_distance(0x01, 0xFF) == 2u);
    CHECK(cp_link_sequence_distance(0xFF, 0x00) == 255u);
    (void)off;
    puts("PASS: outbound ACK removal across modulo-256 sequence wrap");
}

static void test_no_bad_frame_delivered(void)
{
    cp_link_config_t cfg;
    uint8_t valid[CP_LINK_MAX_FRAME];
    uint8_t mutation[CP_LINK_MAX_FRAME];
    const uint8_t good[] = {'G', 'O', 'O', 'D'};
    size_t valid_len;
    size_t mutation_len;
    unsigned round;

    base_config(&cfg);
    for (round = 0; round < 8u; round++) {
        handshake_default(&g_link);
        clear_events();
        {
            uint8_t discard[128];
            (void)drain_all(&g_link, discard, sizeof(discard));
        }

        valid_len = make_frame(valid, sizeof(valid), 0x40, 0x2b, 0x63, 3,
                               good, sizeof(good), true);
        memcpy(mutation, valid, valid_len);
        mutation_len = valid_len;
        switch (round) {
        case 0:
            mutation_len = 5u;
            break;
        case 1:
            mutation[8] ^= 0x40u;
            break;
        case 2:
            mutation[valid_len - 1u] ^= 0x01u;
            break;
        case 3:
            mutation_len = make_frame(mutation, sizeof(mutation), 0x40, 0x2b,
                                      0x63, 3, NULL, 0, false);
            mutation[2] = 0u;
            mutation[3] = 8u;
            resign_header(mutation);
            mutation_len = 9u;
            break;
        case 4:
            mutation_len = make_frame(mutation, sizeof(mutation), 0x40, 0x2b,
                                      0x63, 3, NULL, 0, false);
            mutation[2] = 0u;
            mutation[3] = 75u;
            resign_header(mutation);
            mutation_len = 9u;
            break;
        case 5:
            mutation_len = make_frame(mutation, sizeof(mutation), 0x08, 0x2b,
                                      0x63, 3, good, sizeof(good), true);
            break;
        case 6:
            memset(mutation, 0xA5, 32);
            mutation_len = 32u;
            break;
        default:
            mutation[0] = 0xFEu;
            break;
        }

        feed_bytes(&g_link, mutation, mutation_len, 0);
        CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 0u);
        CHECK(cp_link_state(&g_link) == CP_LINK_STATE_NORMAL);

        if (round != 0) {
            feed_frame(&g_link, valid, valid_len, 0);
            CHECK(count_events(CP_LINK_EVENT_SESSION_DATA) == 1u);
            CHECK(last_event(CP_LINK_EVENT_SESSION_DATA)->len == sizeof(good));
            CHECK(memcmp(last_event(CP_LINK_EVENT_SESSION_DATA)->data, good,
                         sizeof(good)) == 0);
        }
    }
    puts("PASS: truncated, corrupt, oversized and unknown-control frames are never delivered");
}

static void test_default_config_bounds(void)
{
    cp_link_config_t cfg;
    const cp_link_sync_t *local;

    cp_link_config_default(&cfg);
    CHECK(cfg.initial_seq == CP_LINK_DEFAULT_INITIAL_SEQ);
    CHECK(cfg.max_payload <= CP_LINK_MAX_PAYLOAD);
    CHECK(cfg.max_payload > 0u);
    CHECK(cfg.max_outgoing <= CP_LINK_MAX_OUTSTANDING);
    CHECK(cfg.max_outgoing > 0u);
    CHECK(cfg.detect_interval_ms > 0u);
    CHECK(cfg.negotiate_interval_ms > 0u);
    CHECK(cfg.handshake_timeout_ms > 0u);
    CHECK(cfg.detect_retry_limit <= 32u);
    CHECK(cfg.negotiate_retry_limit <= 32u);
    CHECK(cfg.retransmission_timeout_ms > 0u);
    CHECK(cfg.max_retransmissions > 0u);
    CHECK(cp_link_reset_reason_string(CP_LINK_RESET_PEER) != NULL);
    CHECK(strcmp(cp_link_reset_reason_string(CP_LINK_RESET_PEER),
                 "peer-reset") == 0);

    /* ZeroACK is reflected in the local advertisement, not just behaviour. */
    cfg.zero_ack = true;
    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    local = cp_link_local_sync(&g_link);
    CHECK(local != NULL);
    CHECK(local->retransmission_timeout_ms == 0u);
    CHECK(local->ack_timeout_ms == 0u);
    CHECK(local->max_retransmissions == 0u);
    CHECK(local->max_ack == 0u);

    cfg.max_payload = 0;
    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) ==
          CP_LINK_ERR_INVALID);
    CHECK(cp_link_init(&g_link, NULL, NULL, NULL) == CP_LINK_ERR_INVALID);
    puts("PASS: default configuration and error reasons are bounded and explicit");
}

static void test_live_bluetooth_profile(void)
{
    cp_link_config_t cfg;
    const cp_link_sync_t *local;

    cp_link_config_default(&cfg);
    CHECK(cfg.control_version == 1u);
    CHECK(cfg.max_outgoing == (CP_LINK_MAX_OUTSTANDING < 30u
                                   ? (uint8_t)CP_LINK_MAX_OUTSTANDING
                                   : 30u));

    cp_link_config_live_bluetooth(&cfg);
    CHECK(cfg.control_version == 2u);
    CHECK(cfg.max_outgoing == 4u);
    CHECK(cfg.zero_ack == false);

    CHECK(cp_link_init(&g_link, &cfg, capture_event, NULL) == CP_LINK_OK);
    clear_events();
    CHECK(cp_link_start(&g_link, CP_LINK_ROLE_INITIATOR, 0) == CP_LINK_OK);
    expect_wire(&g_link,
                "ff550200ee10"
                "ff5a001d80630000a70104100a0fa001f404030a00020b02010c01020d",
                "live Bluetooth profile marker and local SYN");

    local = cp_link_local_sync(&g_link);
    CHECK(local != NULL);
    CHECK(local->version == 1u);
    CHECK(local->max_outgoing == 4u);
    CHECK(local->max_len == (uint16_t)(cfg.max_payload + 10u));
    CHECK(local->retransmission_timeout_ms == 4000u);
    CHECK(local->ack_timeout_ms == 500u);
    CHECK(local->max_retransmissions == 4u);
    CHECK(local->max_ack == 3u);
    CHECK(local->session_count == 3u);
    CHECK(local->sessions[0].id == 10u &&
          local->sessions[0].type == CP_LINK_SESSION_CONTROL &&
          local->sessions[0].version == 2u);
    CHECK(local->sessions[1].id == 11u &&
          local->sessions[1].type == CP_LINK_SESSION_EXTERNAL_ACCESSORY);
    CHECK(local->sessions[2].id == 12u &&
          local->sessions[2].type == CP_LINK_SESSION_FILE_TRANSFER);
    puts("PASS: live Bluetooth preset pins ctrl v2, max_out 4 and ZeroACK off");
}

static void test_ack_only_cumulative(void)
{
    cp_link_config_t cfg;
    uint8_t ack[CP_LINK_HEADER_LEN];
    size_t ack_len;

    base_config(&cfg);
    handshake_default(&g_link);
    clear_events();
    {
        uint8_t discard[128];
        (void)drain_all(&g_link, discard, sizeof(discard));
    }

    /* The negotiation ACK already contributed one ACK-bearing frame. */
    ack_len = make_frame(ack, sizeof(ack), 0x40, 0, 0x63, 0, NULL, 0, false);
    feed_frame(&g_link, ack, ack_len, 0);
    expect_wire(&g_link, "ff5a000940632a00d1",
                "cumulative ACK after max_ack ACK-bearing frames");
    feed_frame(&g_link, ack, ack_len, 0);
    expect_no_output(&g_link);
    feed_frame(&g_link, ack, ack_len, 0);
    expect_wire(&g_link, "ff5a000940632a00d1",
                "next cumulative ACK window");
    puts("PASS: cumulative ACK counting covers ACK-only frames");
}

int main(void)
{
    test_marker_and_local_syn_vector();
    test_reference_ack_vector();
    test_passive_syn_fields_and_session_direction();
    test_unsupported_syn_sessions();
    test_zero_session_is_sequenced();
    test_corruption_checksums_and_lengths();
    test_fragmentation_coalescing_noise();
    test_partial_output_and_backpressure();
    test_ack_burst_coalesces();
    test_partially_consumed_ack_is_immutable();
    test_full_output_ack_deferred();
    test_window_full_backpressure();
    test_sequence_wrap_and_duplicates();
    test_retransmit_identical_and_ack();
    test_retransmit_refreshes_ack();
    test_retransmit_partial_queue_is_immutable();
    test_eak_and_out_of_window();
    test_eak_recomputes_missing_list();
    test_eak_stale_gap_fill_retracts_frame();
    test_eak_full_output_is_deferred();
    test_eak_partial_output_is_immutable();
    test_eak_partial_truncated_followup();
    test_eak_modulo_and_pending_clear();
    test_eak_peer_max_len_capacity();
    test_eak_ring_wrap_nonhead_compaction();
    test_eak_output_burst_is_deferred();
    test_reset_cancel_and_generation();
    test_handshake_bounds_and_marker();
    test_syn_resend_rebuilds_ack();
    test_zero_ack_mode();
    test_payload_bounds();
    test_outbound_ack_wrap();
    test_no_bad_frame_delivered();
    test_default_config_bounds();
    test_live_bluetooth_profile();
    test_ack_only_cumulative();
    puts("PASS: all cp_link tests");
    return 0;
}
