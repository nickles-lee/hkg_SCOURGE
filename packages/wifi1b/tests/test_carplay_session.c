/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * End-to-end offline tests for cp_session. Two independent cp_link engines
 * stand in for accessory and phone; the fake phone uses protocol CSM facts and
 * a test-only mock auth provider. No sockets, radios, OEM code or production
 * authentication provider are used.
 */
#include "../src/carplay/cp_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define PHONE_TX_QUEUE 8u
#define PHONE_MSG_LOG 64u

typedef struct phone_tx_slot {
    uint16_t len;
    uint16_t sent;
    uint16_t segment_cap; /* 0 = peer maximum */
    uint8_t session_id;   /* used when raw_session is true */
    bool raw_session;
    uint8_t bytes[CP_WIRE_MAX_FRAME];
} phone_tx_slot_t;

typedef struct phone {
    cp_link_t link;
    uint8_t rx[CP_SESSION_RX_BUFFER];
    size_t rx_len;
    bool rx_overflow;
    bool link_up;
    bool reset_seen;
    cp_link_reset_reason_t reset_reason;

    phone_tx_slot_t tx[PHONE_TX_QUEUE];
    uint8_t tx_head;
    uint8_t tx_count;

    bool auto_start;
    bool auto_availability;
    bool send_auth_fail;
    bool sent_start;
    bool sent_ident_accept;
    bool sent_challenge;
    bool sent_auth;
    bool sent_auth_fail_msg;

    bool got_ident;
    bool got_cert;
    bool got_resp;
    bool got_wifi;
    bool got_start;
    unsigned got_start_count;

    unsigned rx_payloads;
    unsigned rx_csm_frames;
    size_t rx_max_payload;

    uint16_t msg_ids[PHONE_MSG_LOG];
    size_t msg_count;
} phone_t;

typedef struct session_event_capture {
    cp_session_event_type_t type;
    cp_session_state_t state;
    cp_session_error_t error;
    cp_boot_event boot_event;
    cp_boot_error boot_error;
    uint32_t generation;
    bool receiver_active;
    bool start_sent;
} session_event_capture_t;

typedef struct auth_fixture {
    unsigned cert_calls;
    unsigned signature_calls;
    int cert_rc;
    int signature_rc;
    size_t cert_len;
    size_t signature_len;
} auth_fixture_t;

typedef struct policy_fixture {
    cp_boot_consent consent;
    bool usb_active;
    /* When set, USB is active only for this exact wired id. */
    const char *usb_true_id;
    char last_usb_id[CP_BOOT_MAX_TRANSPORT_ID + 1u];
    unsigned consent_calls;
    unsigned usb_calls;
} policy_fixture_t;

static cp_session_t g_session;
static phone_t g_phone;
static uint64_t g_now;
static uint64_t g_pump_advance = 5u;
static uint32_t g_ap_wait_ms = 60000u;
static auth_fixture_t g_auth;
static policy_fixture_t g_policy;
static session_event_capture_t g_events[128];
static size_t g_event_count;
static unsigned g_event_counts[16];

static const char *const k_langs[] = {"en", "de"};

/* CSM vectors from the pinned protocol facts. Only message IDs/lengths are
 * recorded in events; test failures never print these bytes. */
static const char *const k_1d00 = "404000061d00";
static const char *const k_1d02 = "404000061d02";
static const char *const k_4e0e =
    "404000264e0e0016000041413a42423a43433a44443a45453a464600000a000175"
    "73622d3100";
static const char *const k_aa00 = "40400006aa00";
static const char *const k_aa02 = "4040001eaa0200180000"
    "0102030405060708090a0b0c0d0e0f1011121314";
static const char *const k_aa04 = "40400006aa04";
static const char *const k_aa05 = "40400006aa05";
static const char *const k_4300_wireless =
    "404000254300001f000100050000010016000141413a42423a43433a44443a4545"
    "3a464600";
static const char *const k_4300_both =
    "404000384300001300000005000001000a00017573622d3100001f000100050000"
    "010016000141413a42423a43433a44443a45453a464600";
static const char *const k_4300_both_usb2 =
    "404000384300001300000005000001000a00017573622d3200001f000100050000"
    "010016000141413a42423a43433a44443a45453a464600";
static const char *const k_4300_both_bt_mismatch =
    "404000384300001300000005000001000a00017573622d3100001f000100050000"
    "010016000141413a42423a43433a44443a45453a303000";
static const char *const k_4300_false = "4040000f4300000900010005000000";
static const char *const k_5702 = "404000065702";

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

static int mock_certificate(void *context, uint8_t *out, size_t cap,
                            size_t *out_len)
{
    auth_fixture_t *auth = (auth_fixture_t *)context;
    size_t i;

    auth->cert_calls++;
    if (auth->cert_rc != 0 || out == NULL || out_len == NULL ||
        cap < auth->cert_len)
        return -1;
    for (i = 0; i < auth->cert_len; i++)
        out[i] = (uint8_t)(0xC0u + (uint8_t)i);
    *out_len = auth->cert_len;
    return 0;
}

static int mock_signature(void *context, const uint8_t *challenge,
                          size_t challenge_len, uint8_t *out, size_t cap,
                          size_t *out_len)
{
    auth_fixture_t *auth = (auth_fixture_t *)context;
    size_t i;

    auth->signature_calls++;
    (void)challenge;
    (void)challenge_len;
    if (auth->signature_rc != 0 || out == NULL || out_len == NULL ||
        cap < auth->signature_len)
        return -1;
    for (i = 0; i < auth->signature_len; i++)
        out[i] = (uint8_t)(0x5Au + (uint8_t)i);
    *out_len = auth->signature_len;
    return 0;
}

static cp_boot_consent mock_consent(void *context, uint64_t now_ms)
{
    policy_fixture_t *policy = (policy_fixture_t *)context;
    (void)now_ms;
    policy->consent_calls++;
    return policy->consent;
}

static bool mock_usb_active(void *context, const char *bluetooth_id,
                            const char *usb_id)
{
    policy_fixture_t *policy = (policy_fixture_t *)context;
    size_t i;

    (void)bluetooth_id;
    policy->usb_calls++;
    if (usb_id != NULL) {
        for (i = 0u; i + 1u < sizeof(policy->last_usb_id) &&
                     usb_id[i] != '\0';
             i++)
            policy->last_usb_id[i] = usb_id[i];
        policy->last_usb_id[i] = '\0';
    } else {
        policy->last_usb_id[0] = '\0';
    }
    if (policy->usb_true_id != NULL)
        return usb_id != NULL &&
               strcmp(usb_id, policy->usb_true_id) == 0;
    return policy->usb_active;
}

static cp_boot_config base_boot_config(void)
{
    cp_boot_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "SCOURGE";
    cfg.model_identifier = "GEN5W";
    cfg.manufacturer = "SCOURGE";
    cfg.serial_number = "SN-0001";
    cfg.firmware_version = "1.0.0";
    cfg.hardware_version = "1.0";
    cfg.eap_name = "com.scourge.carplay";
    cfg.current_language = "en";
    cfg.supported_languages = k_langs;
    cfg.supported_language_count = 2u;
    cfg.app_match_team_id = "SCOURGETEAM";
    cfg.bluetooth_transport_name = "bt";
    cfg.bluetooth_mac = "AA:BB:CC:DD:EE:FF";
    cfg.maximum_current_drawn_ma = 0u;
    cfg.ap_ssid = "SCOURGE-AP";
    cfg.ap_passphrase = "Wireless-Test-1";
    cfg.ap_channel = 36u;
    cfg.ap_security = CP_WIRE_WIFI_WPA2;
    cfg.link_local = "fe80::1";
    cfg.airplay_port = 7000u;
    cfg.device_identifier = "SCOURGE-DEV";
    cfg.public_key = "test-public-key";
    cfg.source_version = "1.0.0";
    cfg.ap_wait_ms = g_ap_wait_ms;
    return cfg;
}

static void base_session_config(cp_session_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cp_session_config_default(cfg);
    cfg->link.max_payload = 64u;
    cfg->link.max_outgoing = 8u;
    cfg->link.retransmission_timeout_ms = 1000u;
    cfg->link.ack_timeout_ms = 100u;
    cfg->link.max_retransmissions = 3u;
    cfg->link.max_ack = 2u;
    cfg->link.detect_interval_ms = 1000u;
    cfg->link.detect_retry_limit = 2u;
    cfg->link.negotiate_interval_ms = 500u;
    cfg->link.negotiate_retry_limit = 2u;
    cfg->link.handshake_timeout_ms = 10000u;
    cfg->boot = base_boot_config();
}

static void clear_events(void)
{
    g_event_count = 0;
    memset(g_event_counts, 0, sizeof(g_event_counts));
}

static size_t count_events(cp_session_event_type_t type)
{
    unsigned index = (unsigned)type;
    return index < sizeof(g_event_counts) / sizeof(g_event_counts[0])
               ? g_event_counts[index]
               : 0u;
}

static void session_event(void *user, const cp_session_event_t *event)
{
    session_event_capture_t *capture;

    (void)user;
    if ((unsigned)event->type <
        sizeof(g_event_counts) / sizeof(g_event_counts[0]))
        g_event_counts[(unsigned)event->type]++;
    if (g_event_count >= sizeof(g_events) / sizeof(g_events[0]))
        return;
    capture = &g_events[g_event_count++];
    memset(capture, 0, sizeof(*capture));
    capture->type = event->type;
    capture->state = event->state;
    capture->error = event->error;
    capture->boot_event = event->boot_event;
    capture->boot_error = event->boot_error;
    capture->generation = event->generation;
    capture->receiver_active = event->receiver_active;
    capture->start_sent = event->start_sent;
}

static void session_setup(cp_session_t *session, unsigned link_payload,
                          unsigned link_window)
{
    cp_session_config_t cfg;
    cp_boot_auth_provider provider;

    base_session_config(&cfg);
    cfg.link.max_payload = (uint16_t)link_payload;
    cfg.link.max_outgoing = (uint8_t)link_window;
    CHECK(cp_session_init(session, &cfg, session_event, NULL, g_now) ==
          CP_SESSION_OK);
    memset(&provider, 0, sizeof(provider));
    provider.context = &g_auth;
    provider.certificate = mock_certificate;
    provider.signature = mock_signature;
    CHECK(cp_session_set_auth_provider(session, &provider) == CP_SESSION_OK);
    CHECK(cp_session_set_policy(session, mock_consent, mock_usb_active,
                                &g_policy) == CP_SESSION_OK);
    CHECK(cp_session_start(session, CP_LINK_ROLE_INITIATOR, g_now) ==
          CP_SESSION_OK);
}

static void phone_callback(void *user, const cp_link_event_t *event)
{
    phone_t *phone = (phone_t *)user;

    switch (event->type) {
    case CP_LINK_EVENT_SESSION_DATA:
        if (event->session_type != CP_LINK_SESSION_CONTROL)
            return;
        if (phone->rx_len + (size_t)event->data_len > sizeof(phone->rx)) {
            phone->rx_overflow = true;
            return;
        }
        if (event->data_len != 0u)
            memcpy(phone->rx + phone->rx_len, event->data, event->data_len);
        phone->rx_len += event->data_len;
        phone->rx_payloads++;
        if (event->data_len > phone->rx_max_payload)
            phone->rx_max_payload = event->data_len;
        break;
    case CP_LINK_EVENT_STATE:
        if (event->state == CP_LINK_STATE_NORMAL)
            phone->link_up = true;
        break;
    case CP_LINK_EVENT_RESET:
        phone->reset_seen = true;
        phone->reset_reason = event->reset_reason;
        break;
    default:
        break;
    }
}

static void phone_init(phone_t *phone, unsigned link_payload,
                       unsigned link_window, bool auto_start,
                       bool auto_availability, bool send_auth_fail)
{
    cp_link_config_t cfg;

    memset(phone, 0, sizeof(*phone));
    cp_link_config_default(&cfg);
    cfg.max_payload = (uint16_t)link_payload;
    cfg.max_outgoing = (uint8_t)link_window;
    cfg.retransmission_timeout_ms = 1000u;
    cfg.ack_timeout_ms = 100u;
    cfg.max_retransmissions = 3u;
    cfg.max_ack = 2u;
    cfg.detect_interval_ms = 1000u;
    cfg.detect_retry_limit = 2u;
    cfg.negotiate_interval_ms = 500u;
    cfg.negotiate_retry_limit = 2u;
    cfg.handshake_timeout_ms = 10000u;
    CHECK(cp_link_init(&phone->link, &cfg, phone_callback, phone) ==
          CP_LINK_OK);
    phone->auto_start = auto_start;
    phone->auto_availability = auto_availability;
    phone->send_auth_fail = send_auth_fail;
    CHECK(cp_link_start(&phone->link, CP_LINK_ROLE_PASSIVE, g_now) ==
          CP_LINK_OK);
}

static void phone_tx_pop(phone_t *phone)
{
    memset(&phone->tx[phone->tx_head], 0, sizeof(phone->tx[phone->tx_head]));
    phone->tx_head =
        (uint8_t)(((unsigned)phone->tx_head + 1u) %
                  (unsigned)PHONE_TX_QUEUE);
    phone->tx_count--;
    if (phone->tx_count == 0u)
        phone->tx_head = 0u;
}

static bool phone_queue(phone_t *phone, const uint8_t *bytes, size_t len,
                        uint16_t segment_cap, uint8_t session_id,
                        bool raw_session)
{
    unsigned index;
    phone_tx_slot_t *slot;

    if (bytes == NULL || len == 0u || len > CP_WIRE_MAX_FRAME ||
        phone->tx_count >= PHONE_TX_QUEUE)
        return false;
    index = ((unsigned)phone->tx_head + (unsigned)phone->tx_count) %
            (unsigned)PHONE_TX_QUEUE;
    slot = &phone->tx[index];
    memcpy(slot->bytes, bytes, len);
    slot->len = (uint16_t)len;
    slot->sent = 0u;
    slot->segment_cap = segment_cap;
    slot->session_id = session_id;
    slot->raw_session = raw_session;
    phone->tx_count++;
    return true;
}

static bool phone_queue_hex(phone_t *phone, const char *hex,
                            uint16_t segment_cap)
{
    uint8_t bytes[CP_WIRE_MAX_FRAME];
    size_t len = hex2bin(hex, bytes, sizeof(bytes));

    return phone_queue(phone, bytes, len, segment_cap, 0u, false);
}

static bool phone_queue_hex_many(phone_t *phone,
                                 const char *const *hexes, size_t count,
                                 uint16_t segment_cap)
{
    uint8_t bytes[CP_WIRE_MAX_FRAME];
    size_t total = 0u;
    size_t i;

    for (i = 0; i < count; i++) {
        size_t len = hex2bin(hexes[i], bytes + total, sizeof(bytes) - total);

        total += len;
    }
    return phone_queue(phone, bytes, total, segment_cap, 0u, false);
}

static bool phone_flush(phone_t *phone, uint64_t now_ms)
{
    while (phone->tx_count != 0u) {
        phone_tx_slot_t *slot = &phone->tx[phone->tx_head];
        size_t max_payload = cp_link_max_payload(&phone->link);
        size_t cap;
        size_t remaining;

        if (cp_link_state(&phone->link) != CP_LINK_STATE_NORMAL)
            return true;
        if (max_payload == 0u)
            return true;
        cap = slot->segment_cap == 0u ? max_payload
                                      : (size_t)slot->segment_cap;
        if (cap > max_payload)
            cap = max_payload;
        if (cap == 0u)
            return true;
        remaining = (size_t)slot->len - (size_t)slot->sent;
        if (remaining == 0u) {
            phone_tx_pop(phone);
            continue;
        }
        {
            size_t segment = remaining < cap ? remaining : cap;
            int result = slot->raw_session
                             ? cp_link_send_session(
                                   &phone->link, slot->session_id,
                                   slot->bytes + slot->sent, segment, now_ms)
                             : cp_link_send(
                                   &phone->link, CP_LINK_SESSION_CONTROL,
                                   slot->bytes + slot->sent, segment, now_ms);

            if (result == CP_LINK_OK) {
                slot->sent = (uint16_t)(slot->sent + (uint16_t)segment);
                if (slot->sent == slot->len)
                    phone_tx_pop(phone);
                continue;
            }
            if (result == CP_LINK_ERR_BACKPRESSURE)
                return true;
            return false;
        }
    }
    return true;
}

static void phone_record_msg(phone_t *phone, uint16_t msg_id)
{
    if (phone->msg_count < PHONE_MSG_LOG)
        phone->msg_ids[phone->msg_count++] = msg_id;
}

static void phone_service(phone_t *phone, uint64_t now_ms)
{
    static const char *const ident_batch[] = {k_1d02, k_4e0e, k_aa00};
    static const char *const auth_batch[] = {k_aa05, k_4300_wireless, k_5702};

    if (phone->rx_overflow) {
        CHECK(0);
    }
    if (phone->link_up && phone->auto_start && !phone->sent_start) {
        CHECK(phone_queue_hex(phone, k_1d00, 0u));
        phone->sent_start = true;
    }

    while (phone->rx_len >= CP_WIRE_HEADER_LEN) {
        cp_wire_view view;
        uint16_t total = (uint16_t)(((uint16_t)phone->rx[2] << 8) |
                                    (uint16_t)phone->rx[3]);

        CHECK(phone->rx[0] == 0x40u && phone->rx[1] == 0x40u);
        CHECK(total >= CP_WIRE_HEADER_LEN && total <= CP_WIRE_MAX_FRAME);
        if ((size_t)total > phone->rx_len)
            break;
        CHECK(cp_wire_open(phone->rx, total, &view) == CP_WIRE_OK);
        CHECK(view.frame_len == total);
        phone_record_msg(phone, view.msg_id);
        phone->rx_csm_frames++;

        switch (view.msg_id) {
        case 0x1D01u:
            phone->got_ident = true;
            if (!phone->sent_ident_accept) {
                CHECK(phone_queue_hex_many(phone, ident_batch,
                                           sizeof(ident_batch) /
                                               sizeof(ident_batch[0]),
                                           0u));
                phone->sent_ident_accept = true;
            }
            break;
        case 0xAA01u:
            phone->got_cert = true;
            if (phone->send_auth_fail && !phone->sent_auth_fail_msg) {
                CHECK(phone_queue_hex(phone, k_aa04, 0u));
                phone->sent_auth_fail_msg = true;
            } else if (!phone->sent_challenge) {
                CHECK(phone_queue_hex(phone, k_aa02, 7u));
                phone->sent_challenge = true;
            }
            break;
        case 0xAA03u:
            phone->got_resp = true;
            if (!phone->sent_auth) {
                if (phone->auto_availability) {
                    CHECK(phone_queue_hex_many(phone, auth_batch,
                                               sizeof(auth_batch) /
                                                   sizeof(auth_batch[0]),
                                               0u));
                } else {
                    CHECK(phone_queue_hex(phone, k_aa05, 0u));
                }
                phone->sent_auth = true;
            }
            break;
        case 0x5703u:
            phone->got_wifi = true;
            break;
        case 0x4301u:
            phone->got_start = true;
            phone->got_start_count++;
            break;
        default:
            break;
        }

        memmove(phone->rx, phone->rx + total, phone->rx_len - total);
        phone->rx_len -= total;
    }
    CHECK(phone_flush(phone, now_ms));
}

static cp_session_error_t transfer_phone_to_accessory(cp_session_t *session,
                                                      phone_t *phone,
                                                      size_t chunk)
{
    cp_session_error_t err = CP_SESSION_OK;

    while (phone->tx_count != 0u || phone->rx_len != 0u ||
           cp_link_output_pending(&phone->link) != 0u) {
        const uint8_t *data = NULL;
        size_t len = cp_link_output_peek(&phone->link, &data);

        if (len == 0u)
            break;
        if (len > chunk)
            len = chunk;
        err = cp_session_feed(session, data, len, g_now);
        CHECK(cp_link_output_consume(&phone->link, len) == CP_LINK_OK);
        if (err != CP_SESSION_OK)
            return err;
    }
    phone_service(phone, g_now);
    return CP_SESSION_OK;
}

static cp_session_error_t transfer_accessory_to_phone(cp_session_t *session,
                                                      phone_t *phone,
                                                      size_t chunk)
{
    const uint8_t *data = NULL;
    size_t len = cp_session_output_peek(session, &data);

    if (len == 0u)
        return CP_SESSION_OK;
    if (len > chunk)
        len = chunk;
    CHECK(cp_link_feed(&phone->link, data, len, g_now) == CP_LINK_OK);
    {
        cp_session_error_t err = cp_session_output_consume(session, len);
        phone_service(phone, g_now);
        return err;
    }
}

static void pump_step(cp_session_t *session, phone_t *phone, size_t chunk,
                      bool drain_accessory)
{
    cp_session_error_t err;

    g_now += g_pump_advance;
    err = transfer_phone_to_accessory(session, phone, chunk);
    CHECK(err == CP_SESSION_OK);
    if (drain_accessory) {
        err = transfer_accessory_to_phone(session, phone, chunk);
        CHECK(err == CP_SESSION_OK);
    }
    phone_service(phone, g_now);
    CHECK(cp_session_tick(session, g_now) == CP_SESSION_OK);
    CHECK(cp_link_tick(&phone->link, g_now) == CP_LINK_OK);
    phone_service(phone, g_now);
}

static bool pred_links_normal(void)
{
    return cp_session_link_state(&g_session) == CP_LINK_STATE_NORMAL &&
           cp_link_state(&g_phone.link) == CP_LINK_STATE_NORMAL;
}

static bool pred_auth_done(void)
{
    return cp_session_auth_state(&g_session) == CP_BOOT_AUTH_SUCCEEDED &&
           g_phone.tx_count == 0u;
}

static bool pred_auth_failed(void)
{
    return cp_session_auth_state(&g_session) == CP_BOOT_AUTH_FAILED;
}

static bool pred_start_done(void)
{
    return g_phone.got_start && cp_session_start_sent(&g_session);
}

static bool pred_got_ident(void)
{
    return g_phone.got_ident;
}

static void pump_until(bool (*predicate)(void), unsigned max_steps,
                       size_t chunk, bool drain_accessory)
{
    unsigned i;

    for (i = 0; i < max_steps && !predicate(); i++)
        pump_step(&g_session, &g_phone, chunk, drain_accessory);
    CHECK(predicate());
}

static void pump_extra(unsigned steps, size_t chunk, bool drain_accessory)
{
    unsigned i;

    for (i = 0; i < steps; i++)
        pump_step(&g_session, &g_phone, chunk, drain_accessory);
}

static void set_ap_ready_named(bool on_air, const char *ssid)
{
    cp_boot_ap_readback readback;

    memset(&readback, 0, sizeof(readback));
    readback.on_air = on_air;
    if (on_air) {
        readback.ssid = ssid;
        readback.channel = 36u;
        readback.security = CP_WIRE_WIFI_WPA2;
        readback.link_local = "fe80::1";
    }
    CHECK(cp_session_set_ap_readback(&g_session, &readback, g_now) ==
          CP_SESSION_OK);
}

static void set_ap_ready(bool on_air)
{
    set_ap_ready_named(on_air, "SCOURGE-AP-LIVE");
}

static void test_end_to_end(void)
{
    unsigned i;
    uint32_t generation;

    g_now = 1000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 300u;
    g_auth.signature_len = 128u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();

    pump_until(pred_links_normal, 4000u, 5u, true);
    phone_service(&g_phone, g_now);
    CHECK(g_phone.sent_start);

    /* Pause accessory output and confirm the identification reply is
     * backpressured and deferred, not truncated. */
    for (i = 0; i < 12u && cp_session_pending_replies(&g_session) == 0u; i++)
        pump_step(&g_session, &g_phone, 5u, false);
    CHECK(cp_session_pending_replies(&g_session) > 0u);
    CHECK(cp_session_output_pending(&g_session) > 0u);
    CHECK(cp_session_stats(&g_session)->link_backpressure_retries > 0u);
    CHECK(cp_session_stats(&g_session)->csm_replies_sent == 0u);

    pump_until(pred_auth_done, 20000u, 5u, true);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_SUCCEEDED);
    CHECK(g_phone.got_ident);
    CHECK(g_phone.got_cert);
    CHECK(g_phone.got_resp);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(!cp_session_receiver_active(&g_session));

    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_ERR_RECEIVER_STATE);

    set_ap_ready(true);
    pump_until(pred_start_done, 8000u, 5u, true);
    CHECK(cp_session_start_sent(&g_session));
    CHECK(cp_session_state(&g_session) ==
          CP_SESSION_STATE_AWAITING_RECEIVER);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(g_phone.got_wifi);

    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation - 1u,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_ERR_GENERATION_MISMATCH);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "wrong-peer", true) ==
          CP_SESSION_ERR_PEER_MISMATCH);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    CHECK(cp_session_receiver_generation(&g_session) == generation);
    CHECK(cp_session_start_sent(&g_session));
    CHECK(cp_session_state(&g_session) ==
          CP_SESSION_STATE_AWAITING_RECEIVER);
    /* Re-publishing the same live AP readback is idempotent. */
    set_ap_ready(true);
    CHECK(cp_session_receiver_active(&g_session));

    /* AP loss clears the receiver observation but does not pretend the start
     * was never sent. */
    set_ap_ready(false);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(cp_session_receiver_generation(&g_session) == 0u);
    CHECK(cp_session_start_sent(&g_session));
    CHECK(cp_session_projection_active(&g_session) == false);
    set_ap_ready(true);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));

    /* A changed live AP readback invalidates the local observation and its
     * bootstrap hold. */
    set_ap_ready_named(true, "SCOURGE-AP-LIVE-2");
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(cp_session_start_sent(&g_session));
    set_ap_ready(true);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));

    CHECK(cp_session_stats(&g_session)->link.frames_valid > 0u);
    CHECK(cp_session_stats(&g_session)->link.bytes_fed > 0u);
    CHECK(cp_session_stats(&g_session)->csm_frames_rx >= 8u);
    CHECK(cp_session_stats(&g_session)->csm_replies_sent >= 5u);
    CHECK(cp_session_stats(&g_session)->csm_segments_sent >
          cp_session_stats(&g_session)->csm_replies_sent);
    CHECK(cp_session_stats(&g_session)->csm_bytes_sent ==
          cp_session_stats(&g_session)->csm_bytes_queued);
    CHECK(cp_session_stats(&g_session)->csm_coalesced_frames >= 2u);
    CHECK(cp_session_stats(&g_session)->csm_fragmented_frames >= 1u);
    CHECK(g_phone.rx_payloads > g_phone.rx_csm_frames);
    CHECK(g_phone.rx_max_payload <= 32u);
    CHECK(count_events(CP_SESSION_EVENT_RECEIVER) >= 3u);
    CHECK(g_phone.got_start_count >= 1u);
    puts("PASS: end-to-end link/bootstrap/auth/AP/5703/4301 and receiver gating");
}

static void test_auth_failure_and_cancel(void)
{
    uint32_t generation;

    g_now = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, false, true);
    session_setup(&g_session, 64u, 8u);
    clear_events();

    pump_until(pred_links_normal, 4000u, 5u, true);
    pump_until(pred_auth_failed, 12000u, 5u, true);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_FAILED);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_FAILED);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_AUTH_FAILED);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!g_phone.got_start);
    CHECK(cp_session_stats(&g_session)->auth_failures >= 1u);

    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_ERR_RECEIVER_STATE);

    pump_extra(100u, 5u, true);
    CHECK(!g_phone.got_start);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_CANCELLED);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_NONE);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_CANCELLED);
    CHECK(cp_session_generation(&g_session) > generation);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    CHECK(cp_session_output_pending(&g_session) == 0u);
    puts("PASS: auth failure leaves no start, cancel clears auth/pending state");
}

static void test_usb_policy_refusal(void)
{
    g_now = 3000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    g_policy.usb_active = true;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();

    pump_until(pred_links_normal, 4000u, 5u, true);
    pump_until(pred_auth_done, 12000u, 5u, true);
    pump_extra(200u, 5u, true);
    CHECK(g_phone.got_wifi);
    CHECK(!g_phone.got_start);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATED);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_POLICY);
    CHECK(cp_session_stats(&g_session)->policy_refusals >= 1u);

    set_ap_ready(true);
    pump_extra(100u, 5u, true);
    CHECK(!g_phone.got_start);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(cp_session_set_receiver_active(&g_session, g_now,
                                         cp_session_generation(&g_session),
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_ERR_RECEIVER_STATE);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: USB policy blocks start and receiver activation");
}

static void drive_to_start(void)
{
    pump_until(pred_links_normal, 4000u, 5u, true);
    pump_until(pred_auth_done, 20000u, 5u, true);
    set_ap_ready(true);
    pump_until(pred_start_done, 10000u, 5u, true);
    CHECK(cp_session_start_sent(&g_session));
}

static void run_active_policy_case(uint64_t start_now, bool usb_case)
{
    uint32_t generation;

    g_now = start_now;
    g_ap_wait_ms = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    generation = cp_session_generation(&g_session);
    if (usb_case) {
        g_policy.usb_active = true;
        CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                             "AA:BB:CC:DD:EE:FF", true) ==
              CP_SESSION_ERR_POLICY);
        g_policy.usb_active = false;
    } else {
        g_policy.consent = CP_BOOT_CONSENT_DENIED;
        CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                             "AA:BB:CC:DD:EE:FF", true) ==
              CP_SESSION_ERR_POLICY);
        g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    }
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    CHECK(g_session.boot.offer_active);
    CHECK(cp_session_next_deadline(&g_session) <=
          g_now + CP_SESSION_POLICY_RECHECK_MS);

    /* The trusted active observation retires the bootstrap offer timer.
     * Ticking well past ap_wait_ms must not cancel a legitimate projection. */
    g_now = g_session.boot.offer_deadline_ms + 100u;
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    CHECK(cp_session_start_sent(&g_session));
    CHECK(cp_session_state(&g_session) ==
          CP_SESSION_STATE_AWAITING_RECEIVER);

    if (usb_case) {
        g_policy.usb_active = true;
    } else {
        g_policy.consent = CP_BOOT_CONSENT_DENIED;
    }
    g_now += CP_SESSION_POLICY_RECHECK_MS + 1u;
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_POLICY);
    CHECK(cp_session_stats(&g_session)->policy_refusals >= 1u);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATED);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    g_ap_wait_ms = 60000u;
}

static void run_offer_expiry_case(uint64_t start_now)
{
    g_now = start_now;
    g_ap_wait_ms = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    CHECK(cp_session_start_sent(&g_session));
    CHECK(g_session.boot.offer_active);

    g_now = g_session.boot.offer_deadline_ms + 100u;
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATED);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_TIMEOUT);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    g_ap_wait_ms = 60000u;
}

static void test_offer_timer_and_policy_recheck(void)
{
    run_active_policy_case(10000u, true);
    run_active_policy_case(20000u, false);
    run_offer_expiry_case(30000u);
    puts("PASS: active receiver survives ap_wait expiry; USB/consent revoke tears down; unobserved offer expires");
}

static void test_disconnect_and_transport_reset(void)
{
    uint32_t generation;

    g_now = 4000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 32u;
    g_auth.signature_len = 32u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    generation = cp_session_generation(&g_session);

    CHECK(cp_link_abort(&g_phone.link, g_now,
                        CP_LINK_RESET_LOCAL_ABORT) == CP_LINK_OK);
    for (unsigned i = 0; i < 200u &&
                        cp_session_state(&g_session) !=
                            CP_SESSION_STATE_DISCONNECTED;
         i++)
        pump_step(&g_session, &g_phone, 5u, false);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_DISCONNECTED);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_NONE);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(cp_session_generation(&g_session) > generation);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_LINK);

    CHECK(cp_session_transport_reset(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_DISCONNECTED);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    puts("PASS: peer RST and transport reset clear generation-bound state");
}

static void test_reconnect_no_old_replies(void)
{
    uint32_t generation_before;
    size_t i;
    bool saw_old = false;

    g_now = 5000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 24u;
    g_auth.signature_len = 24u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 2u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();

    pump_until(pred_links_normal, 4000u, 5u, true);
    pump_until(pred_auth_done, 20000u, 5u, true);
    CHECK(!cp_session_start_sent(&g_session));

    /* Generate a start reply but leave it partially queued in the link. */
    set_ap_ready(true);
    CHECK(cp_session_pending_replies(&g_session) > 0u ||
          cp_session_output_pending(&g_session) > 0u);
    generation_before = cp_session_generation(&g_session);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_output_pending(&g_session) == 0u);
    CHECK(cp_session_generation(&g_session) > generation_before);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_NONE);

    /* New transport generation: no old credentials/start CSM may appear. */
    phone_init(&g_phone, 32u, 2u, true, true, false);
    CHECK(cp_session_start(&g_session, CP_LINK_ROLE_INITIATOR, g_now) ==
          CP_SESSION_OK);
    CHECK(cp_session_generation(&g_session) > generation_before);
    pump_until(pred_got_ident, 12000u, 5u, true);
    CHECK(g_phone.msg_count >= 1u);
    CHECK(g_phone.msg_ids[0] == 0x1D01u);
    for (i = 0; i < g_phone.msg_count; i++) {
        if (g_phone.msg_ids[i] == 0x4301u ||
            g_phone.msg_ids[i] == 0x5703u ||
            g_phone.msg_ids[i] == 0xAA01u ||
            g_phone.msg_ids[i] == 0xAA03u)
            saw_old = true;
    }
    CHECK(!saw_old);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_NONE);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: reconnect advances generation and leaks no old CSM replies");
}

static void test_malformed_csm(void)
{
    static const uint8_t malformed[6] = {0x40u, 0x40u, 0xFFu, 0xFFu,
                                         0x43u, 0x00u};
    cp_session_error_t err = CP_SESSION_OK;
    uint32_t generation;

    g_now = 6000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 64u, 4u, false, false, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);
    generation = cp_session_generation(&g_session);

    CHECK(phone_queue(&g_phone, malformed, sizeof(malformed), 0u, 0u, false));
    CHECK(phone_flush(&g_phone, g_now));
    while (cp_link_output_pending(&g_phone.link) != 0u) {
        const uint8_t *data = NULL;
        size_t len = cp_link_output_peek(&g_phone.link, &data);

        if (len > 5u)
            len = 5u;
        err = cp_session_feed(&g_session, data, len, g_now);
        CHECK(cp_link_output_consume(&g_phone.link, len) == CP_LINK_OK);
        if (err != CP_SESSION_OK)
            break;
    }
    CHECK(err == CP_SESSION_ERR_CSM_MALFORMED);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_FAILED);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_CSM_MALFORMED);
    CHECK(cp_session_stats(&g_session)->csm_malformed == 1u);
    CHECK(cp_session_generation(&g_session) > generation);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);

    /* AP readback publication must also preserve a failed lifecycle. */
    set_ap_ready(false);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_FAILED);
    set_ap_ready(true);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_FAILED);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: malformed CSM length aborts explicitly without payload processing");
}

static void test_tx_queue_overflow(void)
{
    uint8_t requests[9u * 6u];
    uint8_t one[6];
    size_t i;
    size_t one_len;
    cp_session_error_t err = CP_SESSION_OK;
    uint32_t generation;

    g_now = 7000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 64u, 4u, true, false, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);
    pump_until(pred_auth_done, 12000u, 5u, true);
    CHECK(cp_session_pending_replies(&g_session) == 0u);

    one_len = hex2bin(k_5702, one, sizeof(one));
    for (i = 0; i < 9u; i++)
        memcpy(requests + i * one_len, one, one_len);
    CHECK(phone_queue(&g_phone, requests, sizeof(requests), 0u, 0u, false));
    generation = cp_session_generation(&g_session);
    CHECK(phone_flush(&g_phone, g_now));
    while (cp_link_output_pending(&g_phone.link) != 0u) {
        const uint8_t *data = NULL;
        size_t len = cp_link_output_peek(&g_phone.link, &data);

        if (len > 5u)
            len = 5u;
        err = cp_session_feed(&g_session, data, len, g_now);
        CHECK(cp_link_output_consume(&g_phone.link, len) == CP_LINK_OK);
        if (err != CP_SESSION_OK)
            break;
    }
    CHECK(err == CP_SESSION_ERR_TX_QUEUE_OVERFLOW);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_FAILED);
    CHECK(cp_session_last_error(&g_session) ==
          CP_SESSION_ERR_TX_QUEUE_OVERFLOW);
    CHECK(cp_session_stats(&g_session)->csm_tx_queue_overflows >= 1u);
    CHECK(cp_session_generation(&g_session) > generation);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    puts("PASS: full CSM reply queue aborts explicitly instead of dropping replies");
}

static void test_unknown_session_cannot_inject_csm(void)
{
    uint8_t csm[16];
    size_t csm_len;
    cp_session_error_t err;

    g_now = 8000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 64u, 4u, false, false, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);

    csm_len = hex2bin(k_1d00, csm, sizeof(csm));
    CHECK(cp_link_send_session(&g_phone.link, 11u, csm, csm_len, g_now) ==
          CP_LINK_OK);
    err = transfer_phone_to_accessory(&g_session, &g_phone, 5u);
    CHECK(err == CP_SESSION_OK);
    CHECK(cp_session_stats(&g_session)->unsupported_session_payloads == 1u);
    CHECK(cp_session_stats(&g_session)->csm_frames_rx == 0u);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_NONE);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_STARTING);
    puts("PASS: non-control session data cannot inject a CSM");
}

static size_t drain_phone_wire(uint8_t *wire, size_t cap)
{
    size_t wire_len = 0u;

    while (cp_link_output_pending(&g_phone.link) != 0u) {
        const uint8_t *data = NULL;
        size_t len = cp_link_output_peek(&g_phone.link, &data);

        CHECK(len != 0u);
        CHECK(wire_len + len <= cap);
        memcpy(wire + wire_len, data, len);
        wire_len += len;
        CHECK(cp_link_output_consume(&g_phone.link, len) == CP_LINK_OK);
    }
    return wire_len;
}

static size_t make_raw_data_frame(uint8_t *out, size_t cap, uint8_t seq,
                                   uint8_t ack, uint8_t session_id,
                                   const uint8_t *payload, size_t payload_len)
{
    size_t total = 9u + payload_len + 1u;
    uint8_t sum = 0u;
    size_t i;

    CHECK(total <= cap);
    out[0] = 0xFFu;
    out[1] = 0x5Au;
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)(total & 0xFFu);
    out[4] = 0x40u;
    out[5] = seq;
    out[6] = ack;
    out[7] = session_id;
    for (i = 0u; i < 8u; i++)
        sum = (uint8_t)(sum + out[i]);
    out[8] = (uint8_t)(0u - (unsigned)sum);
    memcpy(out + 9u, payload, payload_len);
    sum = 0u;
    for (i = 0u; i < payload_len; i++)
        sum = (uint8_t)(sum + payload[i]);
    out[9u + payload_len] = (uint8_t)(0u - (unsigned)sum);
    return total;
}

static void drain_session_output(void)
{
    const uint8_t *data = NULL;
    size_t len;

    while ((len = cp_session_output_peek(&g_session, &data)) != 0u)
        CHECK(cp_session_output_consume(&g_session, len) == CP_SESSION_OK);
}

static void build_unknown_csm(uint8_t *csm, size_t len)
{
    CHECK(len >= 6u && len <= CP_WIRE_MAX_FRAME);
    memset(csm, 0x55, len);
    csm[0] = 0x40u;
    csm[1] = 0x40u;
    csm[2] = (uint8_t)(len >> 8);
    csm[3] = (uint8_t)(len & 0xFFu);
    csm[4] = 0x99u;
    csm[5] = 0x99u;
}

static void test_out_of_order_reorder_burst(void)
{
    static uint8_t payloads[4u * CP_WIRE_MAX_FRAME];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t base;
    int session_id;
    size_t i;

    g_now = 21000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 64u, 32u, false, false, false);
    session_setup(&g_session, CP_LINK_MAX_PAYLOAD, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);
    base = g_session.link.last_received_psn;
    session_id = cp_link_session_id(&g_session.link, CP_LINK_SESSION_CONTROL);
    CHECK(session_id > 0);

    for (i = 0u; i < 4u; i++)
        build_unknown_csm(payloads + i * CP_WIRE_MAX_FRAME,
                          CP_WIRE_MAX_FRAME);

    /* Feed B, C, D (seq base+2..base+4) before gap A (base+1). Closing A
     * makes cp_link deliver all four callbacks in one feed call. */
    for (i = 0u; i < 3u; i++) {
        size_t len = make_raw_data_frame(
            frame, sizeof(frame), (uint8_t)(base + 2u + (uint8_t)i), base,
            (uint8_t)session_id, payloads + i * CP_WIRE_MAX_FRAME,
            CP_WIRE_MAX_FRAME);

        CHECK(cp_session_feed(&g_session, frame, len, g_now) == CP_SESSION_OK);
        drain_session_output();
    }
    {
        size_t len = make_raw_data_frame(
            frame, sizeof(frame), (uint8_t)(base + 1u), base,
            (uint8_t)session_id, payloads + 3u * CP_WIRE_MAX_FRAME,
            CP_WIRE_MAX_FRAME);

        CHECK(cp_session_feed(&g_session, frame, len, g_now) == CP_SESSION_OK);
    }
    CHECK(cp_session_stats(&g_session)->csm_frames_rx == 4u);
    CHECK(cp_session_stats(&g_session)->csm_rx_overflows == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    CHECK(cp_session_state(&g_session) != CP_SESSION_STATE_FAILED);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: gap-closing reorder burst delivers four maximum CSMs safely");
}

static void test_reorder_bound_with_partial_csm(void)
{
    static uint8_t stream[CP_SESSION_RX_BUFFER];
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t base;
    uint8_t checksum = 0u;
    uint8_t checksum_frame_payload[1];
    int session_id;
    size_t full_count = (size_t)CP_LINK_MAX_REORDER + 1u;
    size_t i;

    _Static_assert(CP_SESSION_RX_BUFFER ==
                       (CP_LINK_MAX_REORDER + 1u) * CP_LINK_MAX_PAYLOAD +
                           (CP_WIRE_MAX_FRAME - 1u),
                   "reorder-bounded RX buffer");

    g_now = 22000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 64u, 32u, false, false, false);
    session_setup(&g_session, CP_LINK_MAX_PAYLOAD, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);
    base = g_session.link.last_received_psn;
    session_id = cp_link_session_id(&g_session.link, CP_LINK_SESSION_CONTROL);
    CHECK(session_id > 0);

    /* 33 maximum CSMs plus a 4095-byte partial CSM. The last gap-closing byte
     * delivers the gap plus 32 reordered payloads, filling the exact bound. */
    for (i = 0u; i < full_count; i++)
        build_unknown_csm(stream + i * CP_WIRE_MAX_FRAME, CP_WIRE_MAX_FRAME);
    {
        uint8_t *partial = stream + full_count * CP_WIRE_MAX_FRAME;

        memset(partial, 0x55, CP_WIRE_MAX_FRAME - 1u);
        partial[0] = 0x40u;
        partial[1] = 0x40u;
        partial[2] = 0x10u;
        partial[3] = 0x00u; /* declares 4096, only 4095 bytes present */
        partial[4] = 0x99u;
        partial[5] = 0x99u;
    }
    for (i = 0u; i < CP_WIRE_MAX_FRAME - 1u; i++)
        checksum = (uint8_t)(checksum +
                             stream[full_count * CP_WIRE_MAX_FRAME + i]);
    checksum = (uint8_t)(0u - (unsigned)checksum);
    checksum_frame_payload[0] = checksum;

    /* In-order first 4095 bytes of CSM 0; leaves a partial CSM. */
    {
        size_t len = make_raw_data_frame(
            frame, sizeof(frame), (uint8_t)(base + 1u), base,
            (uint8_t)session_id, stream, CP_WIRE_MAX_FRAME - 1u);

        CHECK(cp_session_feed(&g_session, frame, len, g_now) == CP_SESSION_OK);
        drain_session_output();
    }
    CHECK(cp_session_pending_rx(&g_session) == CP_WIRE_MAX_FRAME - 1u);

    /* Buffer seq base+3..base+34 (32 frames); gap base+2 is still missing. */
    for (i = 0u; i < CP_LINK_MAX_REORDER; i++) {
        const uint8_t *payload = stream + 8191u + i * CP_WIRE_MAX_FRAME;
        size_t len = make_raw_data_frame(
            frame, sizeof(frame), (uint8_t)(base + 3u + (uint8_t)i), base,
            (uint8_t)session_id, payload, CP_WIRE_MAX_FRAME);

        CHECK(cp_session_feed(&g_session, frame, len, g_now) == CP_SESSION_OK);
        drain_session_output();
    }
    CHECK(cp_session_pending_rx(&g_session) == CP_WIRE_MAX_FRAME - 1u);

    /* Close the gap: current base+2 plus 32 buffered payloads. */
    {
        const uint8_t *payload = stream + 4095u;
        size_t len = make_raw_data_frame(
            frame, sizeof(frame), (uint8_t)(base + 2u), base,
            (uint8_t)session_id, payload, CP_WIRE_MAX_FRAME);

        CHECK(cp_session_feed(&g_session, frame, len, g_now) == CP_SESSION_OK);
    }
    CHECK(cp_session_stats(&g_session)->csm_frames_rx == full_count);
    CHECK(cp_session_stats(&g_session)->csm_rx_overflows == 0u);
    CHECK(cp_session_stats(&g_session)->csm_malformed == 0u);
    CHECK(cp_session_stats(&g_session)->csm_fragmented_frames >= 1u);
    CHECK(cp_session_stats(&g_session)->csm_coalesced_frames >=
          CP_LINK_MAX_REORDER);
    CHECK(cp_session_pending_rx(&g_session) == CP_WIRE_MAX_FRAME - 1u);
    CHECK(cp_session_state(&g_session) != CP_SESSION_STATE_FAILED);

    /* Complete the final partial CSM with its real checksum byte. */
    {
        size_t len = make_raw_data_frame(
            frame, sizeof(frame), (uint8_t)(base + 35u), base,
            (uint8_t)session_id, checksum_frame_payload, 1u);

        CHECK(cp_session_feed(&g_session, frame, len, g_now) == CP_SESSION_OK);
    }
    CHECK(cp_session_stats(&g_session)->csm_frames_rx == full_count + 1u);
    CHECK(cp_session_stats(&g_session)->csm_rx_overflows == 0u);
    CHECK(cp_session_stats(&g_session)->csm_malformed == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: reorder-bound partial CSM burst fits exactly and completes");
}

static void test_rx_bursts_and_linear_feed(void)
{
    uint8_t burst[4u * CP_WIRE_MAX_FRAME];
    uint8_t wire[4u * CP_LINK_MAX_FRAME];
    size_t wire_len;
    size_t i;
    clock_t before;
    clock_t after;
    cp_session_error_t err;

    /* Four maximum CSMs in one transport read. Read size is not a deferred
     * buffer bound: byte-wise feed must service frames between bytes and
     * process all four without RX overflow. */
    g_now = 15000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, CP_LINK_MAX_PAYLOAD, 8u, false, false, false);
    session_setup(&g_session, CP_LINK_MAX_PAYLOAD, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);

    for (i = 0u; i < 4u; i++) {
        uint8_t *f = burst + i * CP_WIRE_MAX_FRAME;

        memset(f, 0x55, CP_WIRE_MAX_FRAME);
        f[0] = 0x40u;
        f[1] = 0x40u;
        f[2] = 0x10u;
        f[3] = 0x00u; /* total 4096 */
        f[4] = 0x99u;
        f[5] = 0x99u; /* unknown but bounded CSM, ignored */
        CHECK(phone_queue(&g_phone, f, CP_WIRE_MAX_FRAME, 0u, 0u, false));
    }
    CHECK(phone_flush(&g_phone, g_now));
    wire_len = drain_phone_wire(wire, sizeof(wire));
    CHECK(wire_len > CP_WIRE_MAX_FRAME + CP_LINK_MAX_PAYLOAD);

    before = clock();
    err = cp_session_feed(&g_session, wire, wire_len, g_now);
    after = clock();
    CHECK(err == CP_SESSION_OK);
    CHECK(after >= before);
    CHECK((double)(after - before) / (double)CLOCKS_PER_SEC < 5.0);
    CHECK(cp_session_stats(&g_session)->csm_frames_rx == 4u);
    CHECK(cp_session_stats(&g_session)->csm_rx_overflows == 0u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    CHECK(cp_session_state(&g_session) != CP_SESSION_STATE_FAILED);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);

    /* 2048 short CSMs (>8 KiB) in one read, split across three 4096-byte link
     * payloads. Boundaries deliberately split CSMs, exercising reassembly and
     * coalescing together without enlarging the RX buffer. */
    g_now = 16000u;
    phone_init(&g_phone, CP_LINK_MAX_PAYLOAD, 8u, false, false, false);
    session_setup(&g_session, CP_LINK_MAX_PAYLOAD, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);

    for (i = 0u; i < 2048u; i++) {
        uint8_t *f = burst + i * 6u;

        f[0] = 0x40u;
        f[1] = 0x40u;
        f[2] = 0x00u;
        f[3] = 0x06u;
        f[4] = 0x99u;
        f[5] = 0x99u;
    }
    for (i = 0u; i < 3u; i++) {
        CHECK(phone_queue(&g_phone, burst + i * CP_LINK_MAX_PAYLOAD,
                          CP_LINK_MAX_PAYLOAD, 0u, 0u, false));
    }
    CHECK(phone_flush(&g_phone, g_now));
    wire_len = drain_phone_wire(wire, sizeof(wire));
    CHECK(wire_len > CP_WIRE_MAX_FRAME + CP_LINK_MAX_PAYLOAD);

    before = clock();
    err = cp_session_feed(&g_session, wire, wire_len, g_now);
    after = clock();
    CHECK(err == CP_SESSION_OK);
    CHECK(after >= before);
    CHECK((double)(after - before) / (double)CLOCKS_PER_SEC < 5.0);
    CHECK(cp_session_stats(&g_session)->csm_frames_rx == 2048u);
    CHECK(cp_session_stats(&g_session)->csm_rx_overflows == 0u);
    CHECK(cp_session_stats(&g_session)->csm_fragmented_frames >= 1u);
    CHECK(cp_session_stats(&g_session)->csm_coalesced_frames >= 1u);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    CHECK(cp_session_state(&g_session) != CP_SESSION_STATE_FAILED);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: maximum-CSM and >8 KiB bursts process linearly without RX overflow");
}

static void test_held_offer_duplicate_and_new_wired_identity(void)
{
    uint32_t generation;
    uint64_t old_deadline;
    uint64_t old_boot_generation;
    unsigned old_starts;
    unsigned old_refusals;
    uint32_t old_replies;

    g_now = 18000u;
    g_ap_wait_ms = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    CHECK(cp_bootstrap_offer_held(&g_session.boot));
    CHECK(g_session.boot.offer_hold_generation ==
          cp_bootstrap_generation(&g_session.boot));
    old_deadline = g_session.boot.offer_deadline_ms;
    old_boot_generation = cp_bootstrap_generation(&g_session.boot);
    old_starts = g_phone.got_start_count;
    old_replies = cp_session_stats(&g_session)->csm_replies_queued;

    /* A fresh network availability after the retained offer deadline must not
     * expire/rearm bootstrap's start while the trusted hold is in effect. */
    g_now = old_deadline + 100u;
    CHECK(phone_queue_hex(&g_phone, k_4300_wireless, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    CHECK(cp_session_start_sent(&g_session));
    CHECK(g_session.boot.offer_active && g_session.boot.offer_held);
    CHECK(g_session.boot.offer_deadline_ms == old_deadline);
    CHECK(cp_bootstrap_generation(&g_session.boot) == old_boot_generation);
    CHECK(!g_session.boot.pending_start);
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_stats(&g_session)->csm_replies_queued == old_replies);
    CHECK(cp_session_pending_replies(&g_session) == 0u);

    /* A new wired id while held must reach the USB callback before any
     * duplicate decision.  Deferring the id would hide a local USB
     * projection that the policy recognizes only under the new id. */
    g_policy.usb_true_id = "usb-2";
    memset(g_policy.last_usb_id, 0, sizeof(g_policy.last_usb_id));
    old_starts = g_phone.got_start_count;
    old_refusals = cp_session_stats(&g_session)->policy_refusals;
    old_boot_generation = cp_bootstrap_generation(&g_session.boot);
    CHECK(phone_queue_hex(&g_phone, k_4300_both_usb2, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(!g_session.boot.offer_active);
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(g_session.boot.have_phone_usb_id);
    CHECK(strcmp(g_session.boot.phone_usb_id, "usb-2") == 0);
    CHECK(strcmp(g_policy.last_usb_id, "usb-2") == 0);
    CHECK(cp_session_stats(&g_session)->policy_refusals == old_refusals + 1u);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_POLICY);
    CHECK(cp_bootstrap_generation(&g_session.boot) ==
          old_boot_generation + 1u);
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    g_policy.usb_true_id = NULL;
    g_ap_wait_ms = 60000u;
    puts("PASS: held offer rejects duplicate and new wired id reaches policy");
}

static void test_held_offer_immediate_usb_before_deadline(void)
{
    uint32_t generation;
    uint64_t old_deadline;
    uint64_t old_boot_generation;
    unsigned old_starts;
    unsigned old_refusals;

    g_now = 19000u;
    g_ap_wait_ms = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);

    /* Learn the known wired id before the observation, while only the
     * bounded offer exists. */
    CHECK(phone_queue_hex(&g_phone, k_4300_both, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(g_session.boot.have_phone_usb_id);
    CHECK(strcmp(g_session.boot.phone_usb_id, "usb-1") == 0);
    CHECK(g_session.boot.offer_active);

    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_bootstrap_offer_held(&g_session.boot));
    old_deadline = g_session.boot.offer_deadline_ms;
    CHECK(g_now < old_deadline);

    /* USB becomes active before the retained deadline.  The held availability
     * path must re-query policy and tear down immediately, not wait for the
     * next one-second receiver re-check. */
    g_policy.usb_active = true;
    old_starts = g_phone.got_start_count;
    old_refusals = cp_session_stats(&g_session)->policy_refusals;
    old_boot_generation = cp_bootstrap_generation(&g_session.boot);
    CHECK(phone_queue_hex(&g_phone, k_4300_both, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(!g_session.boot.offer_active);
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(strcmp(g_policy.last_usb_id, "usb-1") == 0);
    CHECK(cp_session_stats(&g_session)->policy_refusals == old_refusals + 1u);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_POLICY);
    CHECK(cp_bootstrap_generation(&g_session.boot) ==
          old_boot_generation + 1u);
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_pending_replies(&g_session) == 0u);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    g_ap_wait_ms = 60000u;
    puts("PASS: held offer re-queries USB policy before deadline immediately");
}

static void test_held_offer_conservative_policy(void)
{
    uint32_t generation;
    unsigned old_starts;
    unsigned old_refusals;

    /* Missing USB callback: wired availability cannot be hidden by the hold. */
    g_now = 20000u;
    g_ap_wait_ms = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_set_policy(&g_session, mock_consent, NULL, &g_policy) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    old_starts = g_phone.got_start_count;
    old_refusals = cp_session_stats(&g_session)->policy_refusals;
    CHECK(phone_queue_hex(&g_phone, k_4300_both, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!g_session.boot.offer_active);
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(cp_session_stats(&g_session)->policy_refusals == old_refusals + 1u);
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_POLICY);
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);

    /* A different wireless peer id is a hard identity refusal, not a held
     * duplicate. */
    g_now = 21000u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_bootstrap_offer_held(&g_session.boot));
    old_starts = g_phone.got_start_count;
    CHECK(phone_queue_hex(&g_phone, k_4300_both_bt_mismatch, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!g_session.boot.offer_active);
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_BOOTSTRAP);
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);

    /* Withdrawal remains visible and clears the hold; later ticks cannot
     * revive the start. */
    g_now = 22000u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    old_starts = g_phone.got_start_count;
    CHECK(phone_queue_hex(&g_phone, k_4300_false, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    CHECK(transfer_phone_to_accessory(&g_session, &g_phone, 5u) ==
          CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!g_session.boot.offer_active);
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(g_phone.got_start_count == old_starts);
    g_now += 5000u;
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    g_ap_wait_ms = 60000u;
    puts("PASS: held offer keeps missing-callback, identity and withdrawal policy");
}

static void test_ap_scope_change_releases_hold(void)
{
    uint32_t generation;
    uint64_t old_boot_generation;
    uint64_t old_deadline;
    unsigned old_starts;

    g_now = 23000u;
    g_ap_wait_ms = 2000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    pump_extra(50u, 5u, true);
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_bootstrap_offer_held(&g_session.boot));
    old_boot_generation = cp_bootstrap_generation(&g_session.boot);
    old_starts = g_phone.got_start_count;

    /* An explicit false observation releases the hold without cancelling the
     * bounded offer; a fresh observation can hold it again. */
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", false) ==
          CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(g_session.boot.offer_active);
    CHECK(cp_bootstrap_generation(&g_session.boot) == old_boot_generation);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_bootstrap_offer_held(&g_session.boot));
    CHECK(cp_bootstrap_generation(&g_session.boot) == old_boot_generation);
    old_deadline = g_session.boot.offer_deadline_ms;

    /* A valid but changed on-air AP is a new scope: the observation and hold
     * drop, while the bounded offer keeps its original deadline/generation. */
    set_ap_ready_named(true, "SCOURGE-AP-LIVE-2");
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));
    CHECK(g_session.boot.offer_active);
    CHECK(g_session.boot.offer_deadline_ms == old_deadline);
    CHECK(cp_bootstrap_generation(&g_session.boot) == old_boot_generation);
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_pending_replies(&g_session) == 0u);

    /* Re-publishing the changed readback does not silently re-hold. */
    set_ap_ready_named(true, "SCOURGE-AP-LIVE-2");
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_bootstrap_offer_held(&g_session.boot));

    /* The offer is still bounded by the original deadline and never starts. */
    g_now = old_deadline + 1u;
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(!g_session.boot.offer_active);
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(g_phone.got_start_count == old_starts);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    g_ap_wait_ms = 60000u;
    puts("PASS: AP scope change releases the hold and cannot revive a start");
}

static void test_t0_policy_recheck_armed(void)
{
    uint32_t generation;

    g_now = 0u;
    g_pump_advance = 0u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 16u;
    g_auth.signature_len = 16u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, true, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    drive_to_start();
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_set_receiver_active(&g_session, g_now, generation,
                                         "AA:BB:CC:DD:EE:FF", true) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));

    /* Replacing policy at t=0 leaves deadline 0; the armed flag must still
     * schedule an immediate re-check. */
    g_policy.consent = CP_BOOT_CONSENT_DENIED;
    CHECK(cp_session_set_policy(&g_session, mock_consent, mock_usb_active,
                                &g_policy) == CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_session));
    CHECK(cp_session_next_deadline(&g_session) == 0u);
    CHECK(cp_session_tick(&g_session, 0u) == CP_SESSION_OK);
    CHECK(!cp_session_receiver_active(&g_session));
    CHECK(!cp_session_start_sent(&g_session));
    CHECK(cp_session_last_error(&g_session) == CP_SESSION_ERR_POLICY);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATED);
    CHECK(cp_session_cancel(&g_session, 0u) == CP_SESSION_OK);
    g_pump_advance = 5u;
    puts("PASS: t=0 policy change is armed and immediately rechecked");
}

static void test_ap_readback_preserves_lifecycle(void)
{
    cp_session_config_t cfg;

    g_now = 19000u;
    memset(&g_auth, 0, sizeof(g_auth));
    memset(&g_policy, 0, sizeof(g_policy));
    base_session_config(&cfg);
    CHECK(cp_session_init(&g_session, &cfg, session_event, NULL, g_now) ==
          CP_SESSION_OK);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_IDLE);
    CHECK(g_session.boot.airplay_port == 7000u);
    set_ap_ready(true);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_IDLE);
    CHECK(g_session.boot.airplay_port == 7000u);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_CANCELLED);
    set_ap_ready(true);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_CANCELLED);
    CHECK(g_session.boot.airplay_port == 7000u);
    set_ap_ready(false);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_CANCELLED);
    CHECK(g_session.boot.airplay_port == 7000u);

    /* Configuration, including the AirPlay port, is fixed per init: a port
     * change requires cancel plus re-init, and the new port is immutable. */
    cfg.boot.airplay_port = 7001u;
    CHECK(cp_session_init(&g_session, &cfg, session_event, NULL, g_now) ==
          CP_SESSION_OK);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_IDLE);
    CHECK(g_session.boot.airplay_port == 7001u);
    set_ap_ready(true);
    CHECK(g_session.boot.airplay_port == 7001u);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    puts("PASS: AP readback publication preserves idle/cancelled lifecycle");
}

static void test_rx_overflow_single_count(void)
{
    uint8_t wire[128];
    size_t wire_len;
    cp_session_error_t err;

    g_now = 20000u;
    memset(&g_auth, 0, sizeof(g_auth));
    g_auth.cert_len = 8u;
    g_auth.signature_len = 8u;
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 64u, 4u, false, false, false);
    session_setup(&g_session, 64u, 8u);
    clear_events();
    pump_until(pred_links_normal, 4000u, 5u, true);

    /* Leave cp_link one byte short of completing a control frame, then fill the
     * deferred buffer. The final byte triggers the callback overflow path; the
     * counter must count one overflow, not callback plus service. */
    CHECK(phone_queue_hex(&g_phone, k_1d00, 0u));
    CHECK(phone_flush(&g_phone, g_now));
    wire_len = drain_phone_wire(wire, sizeof(wire));
    CHECK(wire_len >= 2u);
    CHECK(cp_session_feed(&g_session, wire, wire_len - 1u, g_now) ==
          CP_SESSION_OK);
    g_session.rx_len = sizeof(g_session.rx_buf);
    err = cp_session_feed(&g_session, wire + wire_len - 1u, 1u, g_now);
    CHECK(err == CP_SESSION_ERR_RX_OVERFLOW);
    CHECK(cp_session_stats(&g_session)->csm_rx_overflows == 1u);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_FAILED);
    CHECK(cp_session_pending_rx(&g_session) == 0u);
    puts("PASS: deferred RX overflow is counted exactly once");
}

static void test_clock_and_api_errors(void)
{
    cp_session_config_t cfg;
    cp_session_config_t defaults;
    uint32_t generation;

    g_now = 9000u;
    memset(&g_auth, 0, sizeof(g_auth));
    memset(&g_policy, 0, sizeof(g_policy));
    cp_session_config_default(&defaults);
    CHECK(defaults.link.control_version == 2u);
    CHECK(defaults.link.max_outgoing == 4u);
    CHECK(defaults.link.zero_ack == false);
    CHECK(defaults.link.control_session_id == 10u);
    CHECK(defaults.link.external_accessory_session_id == 11u);
    CHECK(defaults.link.file_transfer_session_id == 12u);
    base_session_config(&cfg);
    CHECK(cp_session_init(&g_session, &cfg, NULL, NULL, g_now) ==
          CP_SESSION_OK);
    clear_events();
    CHECK(cp_session_tick(&g_session, g_now - 1u) ==
          CP_SESSION_ERR_CLOCK_ROLLBACK);
    CHECK(cp_session_stats(&g_session)->clock_rollbacks == 1u);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_IDLE);
    CHECK(cp_session_start(&g_session, CP_LINK_ROLE_INITIATOR, g_now) ==
          CP_SESSION_OK);
    CHECK(cp_session_start(&g_session, CP_LINK_ROLE_INITIATOR, g_now) ==
          CP_SESSION_ERR_STATE);
    CHECK(cp_session_tick(&g_session, g_now - 1u) ==
          CP_SESSION_ERR_CLOCK_ROLLBACK);
    CHECK(cp_session_stats(&g_session)->clock_rollbacks == 2u);
    generation = cp_session_generation(&g_session);
    CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_generation(&g_session) == generation);
    CHECK(cp_session_init(NULL, &cfg, NULL, NULL, g_now) ==
          CP_SESSION_ERR_ARGUMENT);
    CHECK(cp_session_feed(NULL, NULL, 0u, g_now) ==
          CP_SESSION_ERR_ARGUMENT);
    CHECK(cp_session_set_ap_readback(NULL, NULL, g_now) ==
          CP_SESSION_ERR_ARGUMENT);
    cfg.link.max_payload = 18u;
    CHECK(cp_session_init(&g_session, &cfg, NULL, NULL, g_now) ==
          CP_SESSION_ERR_CONFIG);
    base_session_config(&cfg);
    CHECK(cp_session_init(&g_session, &cfg, NULL, NULL, g_now) ==
          CP_SESSION_OK);
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_CANCELLED);
    puts("PASS: monotonic-clock rejection and API argument checks");
}

int main(void)
{
    test_end_to_end();
    test_auth_failure_and_cancel();
    test_usb_policy_refusal();
    test_offer_timer_and_policy_recheck();
    test_disconnect_and_transport_reset();
    test_reconnect_no_old_replies();
    test_malformed_csm();
    test_tx_queue_overflow();
    test_unknown_session_cannot_inject_csm();
    test_rx_bursts_and_linear_feed();
    test_out_of_order_reorder_burst();
    test_reorder_bound_with_partial_csm();
    test_held_offer_duplicate_and_new_wired_identity();
    test_held_offer_immediate_usb_before_deadline();
    test_held_offer_conservative_policy();
    test_ap_scope_change_releases_hold();
    test_t0_policy_recheck_armed();
    test_ap_readback_preserves_lifecycle();
    test_rx_overflow_single_count();
    test_clock_and_api_errors();
    puts("PASS: all cp_session tests");
    return 0;
}
