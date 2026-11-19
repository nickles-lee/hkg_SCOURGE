/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Independent offline checks for the CarPlay CSM codec and wireless bootstrap
 * state machine.  Expected frames are written out from the protocol layout,
 * not produced by the encoder under test.  No vehicle, phone, radio, socket
 * or OEM binary is touched.  "Wireless-Test-1" is a fixture-only fake
 * passphrase, never a credential.
 */

#include <stdio.h>
#include <string.h>

#include "cp_bootstrap.h"
#include "cp_wire.h"

static int failures;
static int checks;

#define CHECK(expr)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(expr)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        checks++;                                                            \
        if (a_ != e_) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s=%lld, expected %lld\n",         \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Small independent helpers                                          */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    size_t i;

    if ((n % 2u) != 0u || n / 2u > cap) {
        return 0u;
    }
    for (i = 0; i < n / 2u; i++) {
        int hi = hexval(hex[2u * i]);
        int lo = hexval(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return 0u;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n / 2u;
}

static int frame_equals(const uint8_t *data, size_t len, const char *hex)
{
    uint8_t expected[4096];
    size_t expected_len = unhex(hex, expected, sizeof(expected));

    return expected_len == len && len != 0u &&
           memcmp(data, expected, len) == 0;
}

static int tlv_next(const uint8_t **cursor, size_t *remaining, uint16_t *id,
                    const uint8_t **payload, size_t *payload_len)
{
    const uint8_t *p = *cursor;
    size_t left = *remaining;
    size_t total;

    if (left == 0u) {
        return 0;
    }
    if (left < 4u) {
        return -1;
    }
    total = ((size_t)p[0] << 8) | p[1];
    if (total < 4u || total > left) {
        return -1;
    }
    *id = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    *payload = p + 4u;
    *payload_len = total - 4u;
    *cursor = p + total;
    *remaining = left - total;
    return 1;
}

static int find_param(const uint8_t *frame, size_t frame_len, uint16_t wanted,
                      const uint8_t **payload, size_t *payload_len)
{
    const uint8_t *p;
    size_t left;
    uint16_t id;
    int rc;

    if (frame_len < 6u) {
        return -1;
    }
    p = frame + 6u;
    left = frame_len - 6u;
    while ((rc = tlv_next(&p, &left, &id, payload, payload_len)) == 1) {
        if (id == wanted) {
            return 1;
        }
    }
    return rc;
}

static int group_param(const uint8_t *group, size_t group_len, uint16_t wanted,
                       const uint8_t **payload, size_t *payload_len)
{
    const uint8_t *p = group;
    size_t left = group_len;
    uint16_t id;
    int rc;

    while ((rc = tlv_next(&p, &left, &id, payload, payload_len)) == 1) {
        if (id == wanted) {
            return 1;
        }
    }
    return rc;
}

static int frame_has_param(const uint8_t *frame, size_t frame_len,
                           uint16_t wanted)
{
    const uint8_t *payload;
    size_t payload_len;
    return find_param(frame, frame_len, wanted, &payload, &payload_len) == 1;
}

static uint16_t rb16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                           */
/* ------------------------------------------------------------------ */

static const char *const k_langs[] = {"en", "de"};

static const char *k_ident_team =
    "404001001d01000c000053434f5552474500000a000147454e355700000c00"
    "0253434f5552474500000c0003534e2d3030303100000a0004312e302e3000"
    "00080005312e3000000e00061d01aa01aa0357034301001a00071d001d021d"
    "03aa00aa02aa04aa05570243004e0d4e0e0005000800000600090000002600"
    "0a000500000100180001636f6d2e73636f757267652e636172706c61790000"
    "050002040010000b53434f555247455445414d000007000c656e00000a000d"
    "656e00646500001f00110006000000000007000162740000040002000a0003"
    "aabbccddeeff00210018000600000001000f000153434f555247452d415000"
    "0004000200040004";

static const char *k_ident_no_team =
    "404000f01d01000c000053434f5552474500000a000147454e355700000c00"
    "0253434f5552474500000c0003534e2d3030303100000a0004312e302e3000"
    "00080005312e3000000e00061d01aa01aa0357034301001a00071d001d021d"
    "03aa00aa02aa04aa05570243004e0d4e0e0005000800000600090000002600"
    "0a000500000100180001636f6d2e73636f757267652e636172706c61790000"
    "050002040007000c656e00000a000d656e00646500001f0011000600000000"
    "0007000162740000040002000a0003aabbccddeeff00210018000600000001"
    "000f000153434f555247452d4150000004000200040004";

static const char *k_wifi_config =
    "404000335703000f000153434f555247452d41500000140002576972656c657373"
    "2d546573742d310000050003020005000424";

static const char *k_start_wpa2 =
    "40400074430100380001000f000053434f555247452d4150000014000157697265"
    "6c6573732d546573742d31000005000224000c0003666538303a3a310000080002"
    "00001b580010000353434f555247452d4445560000140004746573742d7075626c"
    "69632d6b657900000a0005312e302e3000";

static const char *k_start_transition =
    "404000794301003d0001000f000053434f555247452d4150000014000157697265"
    "6c6573732d546573742d31000005000224000c0003666538303a3a310000050004"
    "030008000200001b580010000353434f555247452d444556000014000474657374"
    "2d7075626c69632d6b657900000a0005312e302e3000";

static const char *k_start_wpa3 =
    "404000794301003d0001000f000053434f555247452d4150000014000157697265"
    "6c6573732d546573742d31000005000224000c0003666538303a3a310000050004"
    "040008000200001b580010000353434f555247452d444556000014000474657374"
    "2d7075626c69632d6b657900000a0005312e302e3000";

static const char *k_start_readback =
    "40400074430100380001000f0000436172506c61792d4150000014000157697265"
    "6c6573732d546573742d31000005000295000c0003666538303a3a310000080002"
    "00001b580010000353434f555247452d4445560000140004746573742d7075626c"
    "69632d6b657900000a0005312e302e3000";

static const char *k_aa01 = "4040000daa01000700000102ff";
static const char *k_aa03 = "4040000eaa0300080000deadbeef";
static const char *k_aa00 = "40400006aa00";
static const char *k_aa02 = "4040001eaa0200180000"
    "0102030405060708090a0b0c0d0e0f1011121314";
static const char *k_aa04 = "40400006aa04";
static const char *k_aa05 = "40400006aa05";
static const char *k_1d00 = "404000061d00";
static const char *k_1d02 = "404000061d02";
static const char *k_1d03_team = "4040000a1d030004000b";
static const char *k_1d03_bt = "4040000a1d0300040011";
static const char *k_4e0d_available = "4040000b4e0d0005000001";
static const char *k_4e0d_unavailable = "4040000b4e0d0005000000";
static const char *k_4e0e =
    "404000264e0e0016000041413a42423a43433a44443a45453a464600000a000175"
    "73622d3100";
static const char *k_4e0e_usb2 =
    "404000264e0e0016000041413a42423a43433a44443a45453a464600000a000175"
    "73622d3200";
static const char *k_4300_both =
    "404000384300001300000005000001000a00017573622d3100001f000100050000"
    "010016000141413a42423a43433a44443a45453a464600";
static const char *k_4300_wireless =
    "404000254300001f000100050000010016000141413a42423a43433a44443a4545"
    "3a464600";
static const char *k_4300_both_usb2 =
    "404000384300001300000005000001000a00017573622d3200001f000100050000"
    "010016000141413a42423a43433a44443a45453a464600";
static const char *k_4300_false = "4040000f4300000900010005000000";
static const char *k_5702 = "404000065702";

static cp_boot_config base_config(void)
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
    cfg.ap_wait_ms = 1000u;
    return cfg;
}

static cp_wire_identification base_ident(void)
{
    cp_wire_identification id;

    memset(&id, 0, sizeof(id));
    id.name = "SCOURGE";
    id.model_identifier = "GEN5W";
    id.manufacturer = "SCOURGE";
    id.serial_number = "SN-0001";
    id.firmware_version = "1.0.0";
    id.hardware_version = "1.0";
    id.eap_name = "com.scourge.carplay";
    id.current_language = "en";
    id.supported_languages = k_langs;
    id.supported_language_count = 2u;
    id.app_match_team_id = "SCOURGETEAM";
    id.bluetooth_transport_name = "bt";
    id.bluetooth_mac[0] = 0xAAu;
    id.bluetooth_mac[1] = 0xBBu;
    id.bluetooth_mac[2] = 0xCCu;
    id.bluetooth_mac[3] = 0xDDu;
    id.bluetooth_mac[4] = 0xEEu;
    id.bluetooth_mac[5] = 0xFFu;
    id.wireless_transport_name = "SCOURGE-AP";
    id.maximum_current_drawn_ma = 0u;
    return id;
}

static cp_wire_start_session base_start(void)
{
    cp_wire_start_session start;

    memset(&start, 0, sizeof(start));
    start.ssid = "SCOURGE-AP";
    start.passphrase = "Wireless-Test-1";
    start.channel = 36u;
    start.security = CP_WIRE_WIFI_WPA2;
    start.link_local = "fe80::1";
    start.port = 7000u;
    start.device_identifier = "SCOURGE-DEV";
    start.public_key = "test-public-key";
    start.source_version = "1.0.0";
    return start;
}

typedef struct {
    int consent;
    bool usb_active;
    const char *usb_true_id;
    int consent_calls;
    int usb_calls;
} policy_fixture;

static cp_boot_consent policy_consent(void *context, uint64_t now_ms)
{
    policy_fixture *p = (policy_fixture *)context;
    (void)now_ms;
    p->consent_calls++;
    return (cp_boot_consent)p->consent;
}

static bool policy_usb(void *context, const char *bluetooth_id,
                       const char *usb_id)
{
    policy_fixture *p = (policy_fixture *)context;
    (void)bluetooth_id;
    p->usb_calls++;
    if (p->usb_true_id != NULL)
        return usb_id != NULL && strcmp(usb_id, p->usb_true_id) == 0;
    return p->usb_active;
}

typedef struct {
    int cert_rc;
    int signature_rc;
    int cert_calls;
    int signature_calls;
} auth_fixture;

static int auth_certificate(void *context, uint8_t *out, size_t cap,
                            size_t *out_len)
{
    auth_fixture *a = (auth_fixture *)context;
    static const uint8_t certificate[] = {0x01u, 0x02u, 0xFFu};

    a->cert_calls++;
    if (a->cert_rc != 0 || cap < sizeof(certificate)) {
        return -1;
    }
    memcpy(out, certificate, sizeof(certificate));
    *out_len = sizeof(certificate);
    return 0;
}

static int auth_signature(void *context, const uint8_t *challenge,
                          size_t challenge_len, uint8_t *out, size_t cap,
                          size_t *out_len)
{
    auth_fixture *a = (auth_fixture *)context;
    static const uint8_t signature[] = {0xDEu, 0xADu, 0xBEu, 0xEFu};

    a->signature_calls++;
    (void)challenge;
    (void)challenge_len;
    if (a->signature_rc != 0 || cap < sizeof(signature)) {
        return -1;
    }
    memcpy(out, signature, sizeof(signature));
    *out_len = sizeof(signature);
    return 0;
}

static cp_boot_result feed(cp_bootstrap *session, const char *hex,
                           uint64_t now_ms, uint8_t *out, size_t out_cap)
{
    uint8_t frame[4096];
    size_t frame_len = unhex(hex, frame, sizeof(frame));
    cp_boot_result result;

    CHECK(frame_len != 0u);
    if (frame_len == 0u) {
        memset(&result, 0, sizeof(result));
        result.event = CP_BOOT_EV_ERROR;
        result.error = CP_BOOT_ERR_ARGUMENT;
        return result;
    }
    return cp_bootstrap_receive(session, frame, frame_len, now_ms, out,
                                out_cap);
}

static void install_policy(cp_bootstrap *session, policy_fixture *policy,
                           int consent, bool usb_active)
{
    policy->consent = consent;
    policy->usb_active = usb_active;
    policy->usb_true_id = NULL;
    policy->consent_calls = 0;
    policy->usb_calls = 0;
    CHECK_EQ(cp_bootstrap_set_policy(session, policy_consent, policy_usb,
                                     policy),
             CP_BOOT_OK);
}

static void install_auth(cp_bootstrap *session, auth_fixture *auth)
{
    cp_boot_auth_provider provider;

    memset(&provider, 0, sizeof(provider));
    provider.context = auth;
    provider.certificate = auth_certificate;
    provider.signature = auth_signature;
    CHECK_EQ(cp_bootstrap_set_auth_provider(session, &provider), CP_BOOT_OK);
}

static cp_boot_result set_ap_readback_fields(cp_bootstrap *session,
                                             uint64_t now_ms, uint8_t *out,
                                             size_t out_cap, const char *ssid,
                                             uint8_t channel,
                                             cp_wire_wifi_security security,
                                             const char *link_local)
{
    cp_boot_ap_readback readback;

    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = ssid;
    readback.channel = channel;
    readback.security = security;
    readback.link_local = link_local;
    return cp_bootstrap_set_ap_readback(session, &readback, now_ms, out,
                                        out_cap);
}

static void set_ap_ready(cp_bootstrap *session, uint64_t now_ms, uint8_t *out,
                         size_t out_cap)
{
    CHECK_EQ(set_ap_readback_fields(session, now_ms, out, out_cap,
                                    "SCOURGE-AP", 36u, CP_WIRE_WIFI_WPA2,
                                    NULL)
                 .event,
             CP_BOOT_EV_AP_READY);
}

static void identify_and_authenticate(cp_bootstrap *session, policy_fixture *p,
                                      auth_fixture *a, uint64_t *now_ms,
                                      uint8_t *out, size_t out_cap)
{
    cp_boot_result result;

    install_policy(session, p, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(session, a);
    result = feed(session, k_1d00, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(session, k_1d02, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = feed(session, k_aa00, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(session, k_aa02, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    result = feed(session, k_aa05, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    *now_ms += 10u;
}

/* ------------------------------------------------------------------ */
/* Wire-level tests                                                   */
/* ------------------------------------------------------------------ */

static void test_headers_and_tlv(void)
{
    uint8_t frame[32];
    size_t frame_len = 0u;
    uint16_t msg_id = 0u;
    cp_wire_view view;
    cp_wire_status st;
    cp_wire_tlv_iter it;
    const uint8_t *payload;
    size_t payload_len;
    uint16_t id;
    uint8_t coalesced[12];

    st = cp_wire_peek(NULL, 6u, &msg_id, &frame_len);
    CHECK_EQ(st, CP_WIRE_ERR_ARGUMENT);
    st = cp_wire_peek(frame, 0u, &msg_id, &frame_len);
    CHECK_EQ(st, CP_WIRE_ERR_TRUNCATED);

    {
        static const uint8_t short_frame[5] = {0x40u, 0x40u, 0x00u, 0x06u,
                                               0xAAu};
        st = cp_wire_peek(short_frame, sizeof(short_frame), &msg_id,
                          &frame_len);
        CHECK_EQ(st, CP_WIRE_ERR_TRUNCATED);
    }
    {
        static const uint8_t bad_start[6] = {0x40u, 0x41u, 0x00u, 0x06u,
                                             0xAAu, 0x00u};
        st = cp_wire_peek(bad_start, sizeof(bad_start), &msg_id, &frame_len);
        CHECK_EQ(st, CP_WIRE_ERR_HEADER);
    }
    {
        static const uint8_t short_len[6] = {0x40u, 0x40u, 0x00u, 0x05u,
                                             0xAAu, 0x00u};
        st = cp_wire_peek(short_len, sizeof(short_len), &msg_id, &frame_len);
        CHECK_EQ(st, CP_WIRE_ERR_LENGTH);
    }
    {
        static const uint8_t huge[6] = {0x40u, 0x40u, 0x10u, 0x01u,
                                        0xAAu, 0x00u};
        st = cp_wire_peek(huge, sizeof(huge), &msg_id, &frame_len);
        CHECK_EQ(st, CP_WIRE_ERR_LENGTH);
    }
    {
        static const uint8_t truncated[10] = {0x40u, 0x40u, 0x00u, 0x10u,
                                              0x43u, 0x00u, 0x00u, 0x04u,
                                              0x00u, 0x00u};
        st = cp_wire_peek(truncated, sizeof(truncated), &msg_id, &frame_len);
        CHECK_EQ(st, CP_WIRE_ERR_TRUNCATED);
    }

    coalesced[0] = 0x40u;
    coalesced[1] = 0x40u;
    coalesced[2] = 0x00u;
    coalesced[3] = 0x06u;
    coalesced[4] = 0xAAu;
    coalesced[5] = 0x00u;
    coalesced[6] = 0x40u;
    coalesced[7] = 0x40u;
    coalesced[8] = 0x00u;
    coalesced[9] = 0x06u;
    coalesced[10] = 0x1Du;
    coalesced[11] = 0x00u;
    st = cp_wire_peek(coalesced, sizeof(coalesced), &msg_id, &frame_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK_EQ(msg_id, 0xAA00u);
    CHECK_EQ(frame_len, 6u);
    st = cp_wire_open(coalesced, sizeof(coalesced), &view);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK_EQ(view.params_len, 0u);
    st = cp_wire_decode_empty(&view, 0xAA00u);
    CHECK_EQ(st, CP_WIRE_OK);
    st = cp_wire_open(coalesced + 6u, sizeof(coalesced) - 6u, &view);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK_EQ(view.msg_id, 0x1D00u);
    st = cp_wire_decode_empty(&view, 0x1D00u);
    CHECK_EQ(st, CP_WIRE_OK);

    {
        static const uint8_t bad_tlv[10] = {0x40u, 0x40u, 0x00u, 0x0Au,
                                            0xAAu, 0x00u, 0x00u, 0x03u,
                                            0x00u, 0x00u};
        st = cp_wire_open(bad_tlv, sizeof(bad_tlv), &view);
        CHECK_EQ(st, CP_WIRE_OK);
        st = cp_wire_tlv_iter_init(&it, view.params, view.params_len);
        CHECK_EQ(st, CP_WIRE_OK);
        st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len);
        CHECK_EQ(st, CP_WIRE_ERR_TLV);
    }
    {
        static const uint8_t trunc_tlv[10] = {0x40u, 0x40u, 0x00u, 0x0Au,
                                              0xAAu, 0x00u, 0x00u, 0x08u,
                                              0x00u, 0x00u};
        st = cp_wire_open(trunc_tlv, sizeof(trunc_tlv), &view);
        CHECK_EQ(st, CP_WIRE_OK);
        st = cp_wire_tlv_iter_init(&it, view.params, view.params_len);
        st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len);
        CHECK_EQ(st, CP_WIRE_ERR_TRUNCATED);
    }
    {
        static const uint8_t trailing[11] = {0x40u, 0x40u, 0x00u, 0x0Bu,
                                             0xAAu, 0x00u, 0x00u, 0x04u,
                                             0x00u, 0x00u, 0x55u};
        st = cp_wire_open(trailing, sizeof(trailing), &view);
        CHECK_EQ(st, CP_WIRE_OK);
        st = cp_wire_tlv_iter_init(&it, view.params, view.params_len);
        st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len);
        CHECK_EQ(st, CP_WIRE_OK);
        st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len);
        CHECK_EQ(st, CP_WIRE_ERR_TLV);
    }
    {
        static const uint8_t extra[10] = {0x40u, 0x40u, 0x00u, 0x0Au,
                                          0xAAu, 0x00u, 0x00u, 0x04u,
                                          0x00u, 0x00u};
        st = cp_wire_open(extra, sizeof(extra), &view);
        CHECK_EQ(st, CP_WIRE_OK);
        st = cp_wire_decode_empty(&view, 0xAA00u);
        CHECK_EQ(st, CP_WIRE_ERR_PARAM);
        st = cp_wire_decode_empty(&view, 0xAA05u);
        CHECK_EQ(st, CP_WIRE_ERR_MSG_ID);
    }

    {
        static const uint8_t bad_mac1[] = "01:BB:CC:DD:EE:FF";
        static const uint8_t bad_mac2[] = "00:00:00:00:00:00";
        static const uint8_t bad_mac3[] = "FF:FF:FF:FF:FF:FF";
        static const uint8_t bad_mac4[] = "AA:BB:CC:DD:EE";
        static const uint8_t bad_mac5[] = "AA-BB-CC-DD-EE-FF";
        uint8_t mac[6];
        CHECK_EQ(cp_wire_parse_bt_mac("AA:BB:CC:DD:EE:FF", mac),
                 CP_WIRE_OK);
        CHECK_EQ(mac[0], 0xAAu);
        CHECK_EQ(mac[5], 0xFFu);
        CHECK_EQ(cp_wire_parse_bt_mac("aa:bb:cc:dd:ee:fe", mac), CP_WIRE_OK);
        CHECK_EQ(cp_wire_parse_bt_mac((const char *)bad_mac1, mac),
                 CP_WIRE_ERR_RANGE);
        CHECK_EQ(cp_wire_parse_bt_mac((const char *)bad_mac2, mac),
                 CP_WIRE_ERR_RANGE);
        CHECK_EQ(cp_wire_parse_bt_mac((const char *)bad_mac3, mac),
                 CP_WIRE_ERR_RANGE);
        CHECK_EQ(cp_wire_parse_bt_mac((const char *)bad_mac4, mac),
                 CP_WIRE_ERR_RANGE);
        CHECK_EQ(cp_wire_parse_bt_mac((const char *)bad_mac5, mac),
                 CP_WIRE_ERR_RANGE);
        CHECK(cp_wire_bt_mac_usable(mac));
    }

    CHECK(cp_wire_link_local_usable("fe80::1"));
    CHECK(cp_wire_link_local_usable("FE80::AB"));
    CHECK(cp_wire_link_local_usable("fe80:0:0:0:0:0:0:1"));
    CHECK(!cp_wire_link_local_usable("fe80::"));
    CHECK(!cp_wire_link_local_usable("2001:db8::1"));
    CHECK(!cp_wire_link_local_usable("fe80::1%eth0"));
    CHECK(!cp_wire_link_local_usable("fe80:::1"));
    CHECK(!cp_wire_link_local_usable("fe80::1:"));
    CHECK(!cp_wire_link_local_usable("fe80:1"));
    CHECK(!cp_wire_link_local_usable("::1"));
    CHECK(!cp_wire_link_local_usable("::fe80:1"));
    CHECK(!cp_wire_link_local_usable("::febf:1"));
    CHECK(!cp_wire_link_local_usable("::fe80:0:0:0:0:1"));
    CHECK(!cp_wire_link_local_usable("127.0.0.1"));
    CHECK(!cp_wire_link_local_usable("fe80::gg"));
    CHECK(!cp_wire_link_local_usable(""));

    {
        static const uint8_t good2[] = {0xC3u, 0xA9u};
        static const uint8_t good3[] = {0xE2u, 0x82u, 0xACu};
        static const uint8_t good4[] = {0xF0u, 0x9Fu, 0x98u, 0x80u};
        static const uint8_t overlong[] = {0xC0u, 0x80u};
        static const uint8_t surrogate[] = {0xEDu, 0xA0u, 0x80u};
        static const uint8_t truncated[] = {0xE2u, 0x82u};
        static const uint8_t nul[] = {'a', 0x00u, 'b'};
        static const uint8_t bad_cont[] = {0xE2u, 0x28u, 0xA1u};
        CHECK(cp_wire_utf8_valid(good2, sizeof(good2)));
        CHECK(cp_wire_utf8_valid(good3, sizeof(good3)));
        CHECK(cp_wire_utf8_valid(good4, sizeof(good4)));
        CHECK(!cp_wire_utf8_valid(overlong, sizeof(overlong)));
        CHECK(!cp_wire_utf8_valid(surrogate, sizeof(surrogate)));
        CHECK(!cp_wire_utf8_valid(truncated, sizeof(truncated)));
        CHECK(!cp_wire_utf8_valid(nul, sizeof(nul)));
        CHECK(!cp_wire_utf8_valid(bad_cont, sizeof(bad_cont)));
    }
}

static void test_identification_encoding(void)
{
    cp_wire_identification id = base_ident();
    uint8_t out[512];
    uint8_t small[262];
    size_t out_len = 0u;
    cp_wire_status st;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *nested;
    size_t nested_len;
    size_t i;
    static const uint16_t sent[] = {0x1D01u, 0xAA01u, 0xAA03u, 0x5703u,
                                    0x4301u};
    static const uint16_t received[] = {0x1D00u, 0x1D02u, 0x1D03u, 0xAA00u,
                                        0xAA02u, 0xAA04u, 0xAA05u, 0x5702u,
                                        0x4300u, 0x4E0Du, 0x4E0Eu};

    memset(out, 0xA5, sizeof(out));
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK_EQ(out_len, 256u);
    CHECK(frame_equals(out, out_len, k_ident_team));

    CHECK_EQ(find_param(out, out_len, 6u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, sizeof(sent));
    for (i = 0; i < sizeof(sent) / sizeof(sent[0]); i++) {
        CHECK_EQ(rb16(payload + 2u * i), sent[i]);
    }
    CHECK_EQ(find_param(out, out_len, 7u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, sizeof(received));
    for (i = 0; i < sizeof(received) / sizeof(received[0]); i++) {
        CHECK_EQ(rb16(payload + 2u * i), received[i]);
    }
    CHECK_EQ(find_param(out, out_len, 8u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 1u);
    CHECK_EQ(payload[0], 0u);
    CHECK_EQ(find_param(out, out_len, 9u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 2u);
    CHECK_EQ(payload[0], 0u);
    CHECK_EQ(payload[1], 0u);
    CHECK_EQ(find_param(out, out_len, 11u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 12u);
    CHECK(memcmp(payload, "SCOURGETEAM\0", 12u) == 0);

    CHECK_EQ(find_param(out, out_len, 17u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 3u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 6u);
    CHECK_EQ(nested[0], 0xAAu);
    CHECK_EQ(nested[5], 0xFFu);
    CHECK_EQ(group_param(payload, payload_len, 2u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 0u);

    /* EAP component 4 (car_play) is omitted rather than emitted as an empty
     * flag, which would encode true. */
    CHECK_EQ(find_param(out, out_len, 10u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 0u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 1u);
    CHECK_EQ(nested[0], 1u);
    CHECK_EQ(group_param(payload, payload_len, 1u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 20u);
    CHECK(memcmp(nested, "com.scourge.carplay\0", 20u) == 0);
    CHECK_EQ(group_param(payload, payload_len, 2u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 1u);
    CHECK_EQ(nested[0], 4u);
    CHECK_EQ(group_param(payload, payload_len, 4u, &nested, &nested_len), 0);

    CHECK_EQ(find_param(out, out_len, 24u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 0u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 2u);
    CHECK_EQ(rb16(nested), 1u);
    CHECK_EQ(group_param(payload, payload_len, 4u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 0u);

    CHECK(!frame_has_param(out, out_len, 14u));
    CHECK(!frame_has_param(out, out_len, 15u));
    CHECK(!frame_has_param(out, out_len, 16u));
    CHECK(!frame_has_param(out, out_len, 20u));
    CHECK(!frame_has_param(out, out_len, 21u));
    CHECK(!frame_has_param(out, out_len, 22u));
    CHECK(!frame_has_param(out, out_len, 30u));

    id.app_match_team_id = NULL;
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(frame_equals(out, out_len, k_ident_no_team));
    CHECK(!frame_has_param(out, out_len, 11u));

    memset(small, 0x5A, sizeof(small));
    id.app_match_team_id = "SCOURGETEAM";
    st = cp_wire_encode_identification(&id, small, 255u, &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_BUFFER);
    CHECK_EQ(out_len, 0u);
    CHECK_EQ(small[255], 0x5Au);
    CHECK_EQ(small[256], 0x5Au);
    CHECK_EQ(small[257], 0x5Au);
    st = cp_wire_encode_identification(&id, small, 256u, &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK_EQ(out_len, 256u);

    id = base_ident();
    id.bluetooth_mac[0] = 0x01u;
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    id = base_ident();
    memset(id.bluetooth_mac, 0, 6u);
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    id = base_ident();
    memset(id.bluetooth_mac, 0xFF, 6u);
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);

    id = base_ident();
    id.name = "bad\xC3\x28name";
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_UTF8);

    id = base_ident();
    id.name =
        "0123456789012345678901234567890123456789012345678901234567890123";
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    id.name =
        "01234567890123456789012345678901234567890123456789012345678901234";
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);

    id = base_ident();
    id.supported_language_count = 0u;
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    id.supported_language_count = 5u;
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    id = base_ident();
    {
        static const char *const bad_langs[] = {"en", "bad\xC3\x28"};
        id.supported_languages = bad_langs;
    }
    st = cp_wire_encode_identification(&id, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_UTF8);
}

static void test_wifi_and_start_encoding(void)
{
    cp_wire_wifi_config cfg;
    cp_wire_start_session start;
    uint8_t out[512];
    uint8_t small[128];
    size_t out_len = 0u;
    cp_wire_status st;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *nested;
    size_t nested_len;

    memset(&cfg, 0, sizeof(cfg));
    cfg.ssid = "SCOURGE-AP";
    cfg.passphrase = "Wireless-Test-1";
    cfg.channel = 36u;
    st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(frame_equals(out, out_len, k_wifi_config));
    CHECK_EQ(find_param(out, out_len, 3u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 1u);
    CHECK_EQ(payload[0], 2u);

    cfg.passphrase = "short";
    st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    {
        char pass63[64];
        char pass64[65];
        memset(pass63, 'a', 63u);
        pass63[63] = '\0';
        memset(pass64, 'a', 64u);
        pass64[64] = '\0';
        cfg.passphrase = pass63;
        st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
        CHECK_EQ(st, CP_WIRE_OK);
        cfg.passphrase = pass64;
        st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
        CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    }
    cfg.passphrase = "Wireless-Test-1";
    cfg.ssid = "01234567890123456789012345678901";
    st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    cfg.ssid = "012345678901234567890123456789012";
    st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    cfg.ssid = "SCOURGE-AP";
    cfg.channel = 0u;
    st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    cfg.channel = 234u;
    st = cp_wire_encode_wifi_config(&cfg, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);

    start = base_start();
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(frame_equals(out, out_len, k_start_wpa2));
    CHECK_EQ(find_param(out, out_len, 1u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 0u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 11u);
    CHECK(memcmp(nested, "SCOURGE-AP\0", 11u) == 0);
    CHECK_EQ(group_param(payload, payload_len, 2u, &nested, &nested_len), 1);
    CHECK_EQ(nested[0], 36u);
    CHECK_EQ(group_param(payload, payload_len, 3u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 8u);
    CHECK(memcmp(nested, "fe80::1\0", 8u) == 0);
    CHECK_EQ(group_param(payload, payload_len, 4u, &nested, &nested_len), 0);
    CHECK_EQ(find_param(out, out_len, 2u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 4u);
    CHECK_EQ(payload[0], 0u);
    CHECK_EQ(payload[1], 0u);
    CHECK_EQ(payload[2], 0x1Bu);
    CHECK_EQ(payload[3], 0x58u);
    CHECK_EQ(find_param(out, out_len, 3u, &payload, &payload_len), 1);
    CHECK(memcmp(payload, "SCOURGE-DEV\0", 12u) == 0);
    CHECK_EQ(find_param(out, out_len, 4u, &payload, &payload_len), 1);
    CHECK(memcmp(payload, "test-public-key\0", 16u) == 0);
    CHECK_EQ(find_param(out, out_len, 5u, &payload, &payload_len), 1);
    CHECK(memcmp(payload, "1.0.0\0", 6u) == 0);

    start.security = CP_WIRE_WIFI_WPA2_WPA3;
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(frame_equals(out, out_len, k_start_transition));
    CHECK_EQ(find_param(out, out_len, 1u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 4u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 1u);
    CHECK_EQ(nested[0], 3u);

    start.security = CP_WIRE_WIFI_WPA3;
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(frame_equals(out, out_len, k_start_wpa3));
    CHECK_EQ(find_param(out, out_len, 1u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 4u, &nested, &nested_len), 1);
    CHECK_EQ(nested[0], 4u);

    start = base_start();
    start.ssid = "CarPlay-AP";
    start.channel = 149u;
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(frame_equals(out, out_len, k_start_readback));

    start = base_start();
    start.link_local = "2001:db8::1";
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    start = base_start();
    start.link_local = "fe80::";
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    start = base_start();
    start.port = 0u;
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    start.port = 65536u;
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    start = base_start();
    start.device_identifier = "";
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    start = base_start();
    start.public_key = NULL;
    start.source_version = NULL;
    st = cp_wire_encode_start_session(&start, out, sizeof(out), &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK(!frame_has_param(out, out_len, 4u));
    CHECK(!frame_has_param(out, out_len, 5u));

    start = base_start();
    memset(small, 0x77, sizeof(small));
    st = cp_wire_encode_start_session(&start, small, 115u, &out_len);
    CHECK_EQ(st, CP_WIRE_ERR_BUFFER);
    CHECK_EQ(out_len, 0u);
    CHECK_EQ(small[115], 0x77u);
    st = cp_wire_encode_start_session(&start, small, 116u, &out_len);
    CHECK_EQ(st, CP_WIRE_OK);
    CHECK_EQ(out_len, 116u);

    {
        static const uint8_t cert[] = {1u, 2u, 0xFFu};
        st = cp_wire_encode_auth_certificate(cert, sizeof(cert), out,
                                             sizeof(out), &out_len);
        CHECK_EQ(st, CP_WIRE_OK);
        CHECK(frame_equals(out, out_len, k_aa01));
        st = cp_wire_encode_auth_certificate(cert, 0u, out, sizeof(out),
                                             &out_len);
        CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    }
    {
        /* CP_WIRE_MAX_AUTH_BLOB is a local fixed-buffer policy, not a wire
         * fact.  The inclusive bound and one-past rejection are exercised. */
        uint8_t big[CP_WIRE_MAX_AUTH_BLOB];
        uint8_t big_out[4096];

        memset(big, 0x5A, sizeof(big));
        st = cp_wire_encode_auth_certificate(big, sizeof(big), big_out,
                                             sizeof(big_out), &out_len);
        CHECK_EQ(st, CP_WIRE_OK);
        CHECK_EQ(out_len, CP_WIRE_MAX_AUTH_BLOB + 10u);
        st = cp_wire_encode_auth_certificate(big, sizeof(big) + 1u, big_out,
                                             sizeof(big_out), &out_len);
        CHECK_EQ(st, CP_WIRE_ERR_RANGE);
        st = cp_wire_encode_auth_response(big, sizeof(big), big_out,
                                          sizeof(big_out), &out_len);
        CHECK_EQ(st, CP_WIRE_OK);
        CHECK_EQ(out_len, CP_WIRE_MAX_AUTH_BLOB + 10u);
    }
    {
        static const uint8_t signature[] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
        st = cp_wire_encode_auth_response(signature, sizeof(signature), out,
                                          sizeof(out), &out_len);
        CHECK_EQ(st, CP_WIRE_OK);
        CHECK(frame_equals(out, out_len, k_aa03));
        st = cp_wire_encode_auth_response(signature, 0u, out, sizeof(out),
                                          &out_len);
        CHECK_EQ(st, CP_WIRE_ERR_RANGE);
    }
}

static void test_inbound_decoding(void)
{
    uint8_t frame[4096];
    size_t frame_len;
    cp_wire_view view;
    cp_wire_wireless_update update;
    cp_wire_transport_ids ids;
    cp_wire_availability availability;
    cp_wire_identification_rejection rejection;
    const uint8_t *challenge;
    size_t challenge_len;

    frame_len = unhex(k_4e0d_available, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_wireless_update(&view, &update), CP_WIRE_OK);
    CHECK(update.available);
    frame_len = unhex(k_4e0d_unavailable, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_wireless_update(&view, &update), CP_WIRE_OK);
    CHECK(!update.available);

    {
        static const uint8_t bad_status[11] = {0x40u, 0x40u, 0x00u, 0x0Bu,
                                               0x4Eu, 0x0Du, 0x00u, 0x05u,
                                               0x00u, 0x00u, 0x02u};
        static const uint8_t dup_status[16] = {
            0x40u, 0x40u, 0x00u, 0x10u, 0x4Eu, 0x0Du, 0x00u, 0x05u,
            0x00u, 0x00u, 0x01u, 0x00u, 0x05u, 0x00u, 0x00u, 0x01u};
        static const uint8_t missing_status[6] = {0x40u, 0x40u, 0x00u, 0x06u,
                                                  0x4Eu, 0x0Du};
        CHECK_EQ(cp_wire_open(bad_status, sizeof(bad_status), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_wireless_update(&view, &update),
                 CP_WIRE_ERR_TYPE);
        CHECK_EQ(cp_wire_open(dup_status, sizeof(dup_status), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_wireless_update(&view, &update),
                 CP_WIRE_ERR_DUPLICATE);
        CHECK_EQ(cp_wire_open(missing_status, sizeof(missing_status), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_wireless_update(&view, &update),
                 CP_WIRE_ERR_MISSING);
    }

    frame_len = unhex(k_4e0e, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids), CP_WIRE_OK);
    CHECK(strcmp(ids.bluetooth_transport_id, "AA:BB:CC:DD:EE:FF") == 0);
    CHECK(strcmp(ids.usb_transport_id, "usb-1") == 0);

    {
        static const uint8_t dup_ids[50] = {
            0x40u, 0x40u, 0x00u, 0x32u, 0x4Eu, 0x0Eu,
            0x00u, 0x16u, 0x00u, 0x00u, 0x41u, 0x41u, 0x3Au, 0x42u,
            0x42u, 0x3Au, 0x43u, 0x43u, 0x3Au, 0x44u, 0x44u, 0x3Au,
            0x45u, 0x45u, 0x3Au, 0x46u, 0x46u, 0x00u,
            0x00u, 0x16u, 0x00u, 0x00u, 0x41u, 0x41u, 0x3Au, 0x42u,
            0x42u, 0x3Au, 0x43u, 0x43u, 0x3Au, 0x44u, 0x44u, 0x3Au,
            0x45u, 0x45u, 0x3Au, 0x46u, 0x46u, 0x00u};
        static const uint8_t bad_nul[24] = {
            0x40u, 0x40u, 0x00u, 0x18u, 0x4Eu, 0x0Eu,
            0x00u, 0x0Au, 0x00u, 0x00u, 0x41u, 0x41u, 0x3Au, 0x42u,
            0x42u, 0x00u,
            0x00u, 0x08u, 0x00u, 0x01u, 0x75u, 0x73u, 0x62u, 0x31u};
        CHECK_EQ(cp_wire_open(dup_ids, sizeof(dup_ids), &view), CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids),
                 CP_WIRE_ERR_DUPLICATE);
        CHECK_EQ(cp_wire_open(bad_nul, sizeof(bad_nul), &view), CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids),
                 CP_WIRE_ERR_UTF8);
    }

    frame_len = unhex(k_4300_both, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_availability(&view, &availability), CP_WIRE_OK);
    CHECK(availability.wired.present);
    CHECK(availability.wired.available_known);
    CHECK(availability.wired.available);
    CHECK(strcmp(availability.wired.transport_id, "usb-1") == 0);
    CHECK(availability.wireless.present);
    CHECK(availability.wireless.available_known);
    CHECK(availability.wireless.available);
    CHECK(strcmp(availability.wireless.transport_id,
                 "AA:BB:CC:DD:EE:FF") == 0);

    frame_len = unhex(k_4300_false, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_availability(&view, &availability), CP_WIRE_OK);
    CHECK(!availability.wireless.available);

    {
        static const uint8_t bad_bool[15] = {
            0x40u, 0x40u, 0x00u, 0x0Fu, 0x43u, 0x00u, 0x00u, 0x09u,
            0x00u, 0x01u, 0x00u, 0x05u, 0x00u, 0x00u, 0x02u};
        static const uint8_t dup_group[47] = {
            0x40u, 0x40u, 0x00u, 0x2Fu, 0x43u, 0x00u, 0x00u, 0x09u,
            0x00u, 0x01u, 0x00u, 0x05u, 0x00u, 0x00u, 0x01u, 0x00u,
            0x09u, 0x00u, 0x01u, 0x00u, 0x05u, 0x00u, 0x00u, 0x01u,
            0x00u, 0x09u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u,
            0x01u, 0x00u, 0x09u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u,
            0x00u, 0x01u, 0x00u, 0x04u, 0x00u, 0x01u, 0x00u};
        static const uint8_t nested_trailing[16] = {
            0x40u, 0x40u, 0x00u, 0x10u, 0x43u, 0x00u, 0x00u, 0x0Au,
            0x00u, 0x01u, 0x00u, 0x05u, 0x00u, 0x00u, 0x01u, 0x55u};
        static const uint8_t no_groups[6] = {0x40u, 0x40u, 0x00u, 0x06u,
                                             0x43u, 0x00u};
        CHECK_EQ(cp_wire_open(bad_bool, sizeof(bad_bool), &view), CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_ERR_TYPE);
        CHECK_EQ(cp_wire_open(dup_group, sizeof(dup_group), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_ERR_DUPLICATE);
        CHECK_EQ(cp_wire_open(nested_trailing, sizeof(nested_trailing), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_ERR_TLV);
        CHECK_EQ(cp_wire_open(no_groups, sizeof(no_groups), &view), CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_ERR_MISSING);
    }

    frame_len = unhex(k_1d03_team, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
             CP_WIRE_OK);
    CHECK_EQ(rejection.flagged, (uint32_t)(1u << 11u));
    frame_len = unhex(k_1d03_bt, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
             CP_WIRE_OK);
    CHECK_EQ(rejection.flagged, (uint32_t)(1u << 17u));

    {
        static const uint8_t nonempty_flag[11] = {
            0x40u, 0x40u, 0x00u, 0x0Bu, 0x1Du, 0x03u, 0x00u, 0x05u,
            0x00u, 0x00u, 0x01u};
        static const uint8_t unknown_flag[10] = {
            0x40u, 0x40u, 0x00u, 0x0Au, 0x1Du, 0x03u, 0x00u, 0x04u,
            0x00u, 0x19u};
        static const uint8_t team_and_unknown[14] = {
            0x40u, 0x40u, 0x00u, 0x0Eu, 0x1Du, 0x03u,
            0x00u, 0x04u, 0x00u, 0x0Bu,
            0x00u, 0x04u, 0x00u, 0x19u};
        static const uint8_t team_twice[14] = {
            0x40u, 0x40u, 0x00u, 0x0Eu, 0x1Du, 0x03u,
            0x00u, 0x04u, 0x00u, 0x0Bu,
            0x00u, 0x04u, 0x00u, 0x0Bu};
        static const uint8_t unknown_truncated[10] = {
            0x40u, 0x40u, 0x00u, 0x0Au, 0x1Du, 0x03u, 0x00u, 0x08u,
            0x00u, 0x19u};
        CHECK_EQ(cp_wire_open(nonempty_flag, sizeof(nonempty_flag), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
                 CP_WIRE_ERR_TYPE);
        CHECK_EQ(cp_wire_open(unknown_flag, sizeof(unknown_flag), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
                 CP_WIRE_OK);
        CHECK_EQ(rejection.flagged, 0u);
        CHECK_EQ(cp_wire_open(team_and_unknown, sizeof(team_and_unknown),
                              &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
                 CP_WIRE_OK);
        CHECK_EQ(rejection.flagged, (uint32_t)(1u << 11u));
        CHECK_EQ(cp_wire_open(team_twice, sizeof(team_twice), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
                 CP_WIRE_ERR_DUPLICATE);
        CHECK_EQ(cp_wire_open(unknown_truncated, sizeof(unknown_truncated),
                              &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_identification_rejection(&view, &rejection),
                 CP_WIRE_ERR_TRUNCATED);
    }

    frame_len = unhex(k_aa02, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_challenge_request(&view, &challenge,
                                              &challenge_len),
             CP_WIRE_OK);
    CHECK_EQ(challenge_len, 20u);
    CHECK_EQ(challenge[0], 1u);
    CHECK_EQ(challenge[19], 0x14u);
    {
        static const uint8_t empty_challenge[6] = {
            0x40u, 0x40u, 0x00u, 0x06u, 0xAAu, 0x02u};
        static const uint8_t short_challenge[10] = {
            0x40u, 0x40u, 0x00u, 0x0Au, 0xAAu, 0x02u,
            0x00u, 0x04u, 0x00u, 0x00u};
        CHECK_EQ(cp_wire_open(empty_challenge, sizeof(empty_challenge), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_challenge_request(&view, &challenge,
                                                  &challenge_len),
                 CP_WIRE_ERR_MISSING);
        CHECK_EQ(cp_wire_open(short_challenge, sizeof(short_challenge), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_challenge_request(&view, &challenge,
                                                  &challenge_len),
                 CP_WIRE_ERR_RANGE);
    }
    frame_len = unhex(k_aa00, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_empty(&view, 0xAA00u), CP_WIRE_OK);
    frame_len = unhex(k_5702, frame, sizeof(frame));
    CHECK_EQ(cp_wire_open(frame, frame_len, &view), CP_WIRE_OK);
    CHECK_EQ(cp_wire_decode_empty(&view, 0x5702u), CP_WIRE_OK);

    /* Unknown optional TLVs are skipped after structural validation, at the
     * top level and inside 0x4300 sides.  Known fields stay mandatory and
     * known duplicates stay rejected. */
    {
        static const uint8_t update_unknown[17] = {
            0x40u, 0x40u, 0x00u, 0x11u, 0x4Eu, 0x0Du,
            0x00u, 0x05u, 0x00u, 0x00u, 0x01u,
            0x00u, 0x06u, 0x00u, 0x07u, 0xAAu, 0xBBu};
        static const uint8_t update_unknown_only[12] = {
            0x40u, 0x40u, 0x00u, 0x0Cu, 0x4Eu, 0x0Du,
            0x00u, 0x06u, 0x00u, 0x07u, 0xAAu, 0xBBu};
        CHECK_EQ(cp_wire_open(update_unknown, sizeof(update_unknown),
                              &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_wireless_update(&view, &update),
                 CP_WIRE_OK);
        CHECK(update.available);
        CHECK_EQ(cp_wire_open(update_unknown_only,
                              sizeof(update_unknown_only), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_wireless_update(&view, &update),
                 CP_WIRE_ERR_MISSING);
    }
    {
        static const uint8_t ids_unknown[43] = {
            0x40u, 0x40u, 0x00u, 0x2Bu, 0x4Eu, 0x0Eu,
            0x00u, 0x16u, 0x00u, 0x00u, 0x41u, 0x41u, 0x3Au, 0x42u,
            0x42u, 0x3Au, 0x43u, 0x43u, 0x3Au, 0x44u, 0x44u, 0x3Au,
            0x45u, 0x45u, 0x3Au, 0x46u, 0x46u, 0x00u,
            0x00u, 0x0Au, 0x00u, 0x01u, 0x75u, 0x73u, 0x62u, 0x2Du,
            0x31u, 0x00u,
            0x00u, 0x05u, 0x00u, 0x07u, 0xFFu};
        static const uint8_t ids_unknown_truncated[16] = {
            0x40u, 0x40u, 0x00u, 0x10u, 0x4Eu, 0x0Eu,
            0x00u, 0x06u, 0x00u, 0x00u, 0x78u, 0x00u,
            0x00u, 0x09u, 0x00u, 0x07u};
        CHECK_EQ(cp_wire_open(ids_unknown, sizeof(ids_unknown), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids), CP_WIRE_OK);
        CHECK(strcmp(ids.bluetooth_transport_id, "AA:BB:CC:DD:EE:FF") == 0);
        CHECK(strcmp(ids.usb_transport_id, "usb-1") == 0);
        CHECK_EQ(cp_wire_open(ids_unknown_truncated,
                              sizeof(ids_unknown_truncated), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids),
                 CP_WIRE_ERR_TRUNCATED);
    }
    {
        static const uint8_t availability_unknown[42] = {
            0x40u, 0x40u, 0x00u, 0x2Au, 0x43u, 0x00u,
            0x00u, 0x1Fu, 0x00u, 0x01u,
            0x00u, 0x05u, 0x00u, 0x00u, 0x01u,
            0x00u, 0x16u, 0x00u, 0x01u, 0x41u, 0x41u, 0x3Au, 0x42u,
            0x42u, 0x3Au, 0x43u, 0x43u, 0x3Au, 0x44u, 0x44u, 0x3Au,
            0x45u, 0x45u, 0x3Au, 0x46u, 0x46u, 0x00u,
            0x00u, 0x05u, 0x00u, 0x07u, 0x00u};
        static const uint8_t nested_unknown[42] = {
            0x40u, 0x40u, 0x00u, 0x2Au, 0x43u, 0x00u,
            0x00u, 0x24u, 0x00u, 0x01u,
            0x00u, 0x05u, 0x00u, 0x00u, 0x01u,
            0x00u, 0x16u, 0x00u, 0x01u, 0x41u, 0x41u, 0x3Au, 0x42u,
            0x42u, 0x3Au, 0x43u, 0x43u, 0x3Au, 0x44u, 0x44u, 0x3Au,
            0x45u, 0x45u, 0x3Au, 0x46u, 0x46u, 0x00u,
            0x00u, 0x05u, 0x00u, 0x09u, 0xAAu};
        static const uint8_t availability_unknown_only[12] = {
            0x40u, 0x40u, 0x00u, 0x0Cu, 0x43u, 0x00u,
            0x00u, 0x06u, 0x00u, 0x07u, 0xAAu, 0xBBu};
        static const uint8_t nested_unknown_truncated[15] = {
            0x40u, 0x40u, 0x00u, 0x0Fu, 0x43u, 0x00u,
            0x00u, 0x09u, 0x00u, 0x01u,
            0x00u, 0x09u, 0x00u, 0x00u, 0x01u};
        CHECK_EQ(cp_wire_open(availability_unknown,
                              sizeof(availability_unknown), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_OK);
        CHECK(availability.wireless.available);
        CHECK_EQ(cp_wire_open(nested_unknown, sizeof(nested_unknown), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_OK);
        CHECK(availability.wireless.available);
        CHECK_EQ(cp_wire_open(availability_unknown_only,
                              sizeof(availability_unknown_only), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_ERR_MISSING);
        CHECK_EQ(cp_wire_open(nested_unknown_truncated,
                              sizeof(nested_unknown_truncated), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_availability(&view, &availability),
                 CP_WIRE_ERR_TRUNCATED);
    }
    {
        static const uint8_t challenge_unknown[35] = {
            0x40u, 0x40u, 0x00u, 0x23u, 0xAAu, 0x02u,
            0x00u, 0x18u, 0x00u, 0x00u,
            0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u,
            0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu, 0x10u,
            0x11u, 0x12u, 0x13u, 0x14u,
            0x00u, 0x05u, 0x00u, 0x03u, 0xFFu};
        static const uint8_t challenge_unknown_only[11] = {
            0x40u, 0x40u, 0x00u, 0x0Bu, 0xAAu, 0x02u,
            0x00u, 0x05u, 0x00u, 0x03u, 0xFFu};
        CHECK_EQ(cp_wire_open(challenge_unknown, sizeof(challenge_unknown),
                              &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_challenge_request(&view, &challenge,
                                                  &challenge_len),
                 CP_WIRE_OK);
        CHECK_EQ(challenge_len, 20u);
        CHECK_EQ(challenge[0], 1u);
        CHECK_EQ(challenge[19], 0x14u);
        CHECK_EQ(cp_wire_open(challenge_unknown_only,
                              sizeof(challenge_unknown_only), &view),
                 CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_challenge_request(&view, &challenge,
                                                  &challenge_len),
                 CP_WIRE_ERR_MISSING);
    }

    /* A 128-character transport identifier is the inclusive bound. */
    {
        uint8_t long_frame[160];
        size_t len;
        size_t i;
        uint8_t *p;

        memset(long_frame, 0, sizeof(long_frame));
        long_frame[0] = 0x40u;
        long_frame[1] = 0x40u;
        long_frame[4] = 0x4Eu;
        long_frame[5] = 0x0Eu;
        p = long_frame + 6u;
        p[0] = 0x00u;
        p[1] = 0x85u; /* 133 = 4 + 128 characters + NUL */
        p[2] = 0x00u;
        p[3] = 0x00u;
        for (i = 0; i < 128u; i++) {
            p[4u + i] = (uint8_t)('a' + (i % 26u));
        }
        p[132] = 0u;
        p = long_frame + 6u + 133u;
        p[0] = 0x00u;
        p[1] = 0x06u;
        p[2] = 0x00u;
        p[3] = 0x01u;
        p[4] = 'x';
        p[5] = 0u;
        len = 6u + 133u + 6u;
        long_frame[2] = (uint8_t)(len >> 8);
        long_frame[3] = (uint8_t)len;
        CHECK_EQ(cp_wire_open(long_frame, len, &view), CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids), CP_WIRE_OK);
        CHECK_EQ(strlen(ids.bluetooth_transport_id), 128u);
        CHECK(strcmp(ids.usb_transport_id, "x") == 0);

        memset(long_frame, 0, sizeof(long_frame));
        long_frame[0] = 0x40u;
        long_frame[1] = 0x40u;
        long_frame[4] = 0x4Eu;
        long_frame[5] = 0x0Eu;
        p = long_frame + 6u;
        p[0] = 0x00u;
        p[1] = 0x86u; /* 134 = 4 + 129 characters + NUL */
        p[2] = 0x00u;
        p[3] = 0x00u;
        for (i = 0; i < 129u; i++) {
            p[4u + i] = 'a';
        }
        p[133] = 0u;
        len = 6u + 134u;
        long_frame[2] = (uint8_t)(len >> 8);
        long_frame[3] = (uint8_t)len;
        CHECK_EQ(cp_wire_open(long_frame, len, &view), CP_WIRE_OK);
        CHECK_EQ(cp_wire_decode_transport_ids(&view, &ids),
                 CP_WIRE_ERR_RANGE);
    }
}

/* ------------------------------------------------------------------ */
/* Bootstrap tests                                                    */
/* ------------------------------------------------------------------ */

static void test_init_validation(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    static const char *const five_langs[] = {"en", "de", "fr", "es", "it"};
    static const char *const one_lang[] = {"en"};

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(NULL, &cfg), CP_BOOT_ERR_ARGUMENT);
    CHECK_EQ(cp_bootstrap_init(&session, NULL), CP_BOOT_ERR_ARGUMENT);
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_DISCONNECTED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_NONE);
    CHECK(!cp_bootstrap_projection_active(&session));

    cfg = base_config();
    cfg.bluetooth_mac = "01:BB:CC:DD:EE:FF";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.bluetooth_mac = "AA:BB:CC:DD:EE:FF:00";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.link_local = "2001:db8::1";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.link_local = "fe80::";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.link_local = "::fe80:1";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.link_local = "::febf:1";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.ap_ssid = "0123456789012345678901234567890123";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.ap_passphrase = "short";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.ap_channel = 0u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.ap_security = (cp_wire_wifi_security)99;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.airplay_port = 0u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.airplay_port = 65536u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.ap_wait_ms = CP_BOOT_MAX_AP_WAIT_MS + 1u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.supported_languages = five_langs;
    cfg.supported_language_count = 5u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.supported_language_count = 0u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.supported_languages = one_lang;
    cfg.supported_language_count = 1u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    cfg = base_config();
    cfg.name = "";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.name = "bad\xC3\x28";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
    cfg = base_config();
    cfg.public_key = NULL;
    cfg.source_version = NULL;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    cfg = base_config();
    cfg.public_key = "bad\xC3\x28";
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_ERR_CONFIG);
}

static void test_identification_flow(void)
{
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 100u;

    memset(&auth, 0, sizeof(auth));
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);

    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    CHECK_EQ(result.state, CP_BOOT_STATE_IDENTIFYING);
    CHECK(frame_equals(out, result.out_len, k_ident_team));
    CHECK(!result.projection_active);

    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    CHECK(frame_equals(out, result.out_len, k_ident_team));

    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    CHECK_EQ(result.state, CP_BOOT_STATE_IDENTIFIED);

    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);

    result = feed(&session, k_ident_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    CHECK_EQ(result.error, CP_BOOT_ERR_WIRE);
    result = feed(&session, k_aa01, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    result = feed(&session, k_wifi_config, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    result = feed(&session, k_start_wpa2, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);

    {
        static const uint8_t unknown[6] = {0x40u, 0x40u, 0x00u, 0x06u,
                                           0x12u, 0x34u};
        result = cp_bootstrap_receive(&session, unknown, sizeof(unknown), now,
                                      out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_UNSUPPORTED);
    }
    {
        static const uint8_t malformed[6] = {0x40u, 0x40u, 0x00u, 0x05u,
                                             0x1Du, 0x00u};
        result = cp_bootstrap_receive(&session, malformed, sizeof(malformed),
                                      now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
        CHECK_EQ(result.error, CP_BOOT_ERR_WIRE);
    }

    result = cp_bootstrap_receive(&session, NULL, 0u, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    result = cp_bootstrap_tick(&session, now - 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    CHECK_EQ(result.error, CP_BOOT_ERR_ARGUMENT);
}

static void test_identification_rejection(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 200u;

    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_RETRY);
    CHECK(frame_equals(out, result.out_len, k_ident_no_team));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_RETRY);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_REJECTED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_NONE);
    /* A failed identification is terminal for this association: 1D02 and a
     * team-only 1D03 must not revive it or emit a new identification. */
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_NONE);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.out_len, 0u);

    /* Terminal identification failure cannot start authentication, WiFi
     * disclosure or a projection offer, even after the AP becomes ready. */
    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, "4040000a1d0300040000", now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_REJECTED);
    CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_NONE);
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    CHECK_EQ(result.out_len, 0u);
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(auth.cert_calls, 0);
    result = feed(&session, k_aa02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(auth.signature_calls, 0);
    result = feed(&session, k_5702, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.out_len, 0u);
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
    CHECK(!cp_bootstrap_projection_active(&session));

    /* An unknown rejection flag is a nonfatal unsupported extension: it must
     * not discard the fields this accessory sent. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, "4040000a1d0300040019", now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_UNSUPPORTED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_IDENTIFYING);
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);

    /* A known team flag next to an unknown flag still performs the bounded
     * team-id retry instead of failing mandatory identification. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, "4040000e1d030004000b00040019", now, out,
                  sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_RETRY);
    CHECK(frame_equals(out, result.out_len, k_ident_no_team));
    CHECK_EQ(result.state, CP_BOOT_STATE_IDENTIFYING);

    {
        static const char *const mandatory[] = {
            "4040000a1d0300040000", /* name */
            "4040000a1d0300040011", /* Bluetooth transport */
            "4040000a1d0300040018", /* wireless transport */
            "4040000a1d0300040006", /* messages sent */
        };
        size_t i;
        for (i = 0; i < sizeof(mandatory) / sizeof(mandatory[0]); i++) {
            cfg = base_config();
            CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
            install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED,
                           false);
            result = feed(&session, k_1d00, now, out, sizeof(out));
            CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
            result = feed(&session, mandatory[i], now, out, sizeof(out));
            CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_REJECTED);
            CHECK_EQ(result.out_len, 0u);
            CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
        }
    }

    cfg = base_config();
    cfg.app_match_team_id = NULL;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_REJECTED);
    CHECK_EQ(result.out_len, 0u);

    {
        static const uint8_t bad_flag[10] = {
            0x40u, 0x40u, 0x00u, 0x0Au, 0x1Du, 0x03u, 0x00u, 0x05u,
            0x00u, 0x00u};
        cfg = base_config();
        CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
        install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
        result = feed(&session, k_1d00, now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
        result = cp_bootstrap_receive(&session, bad_flag, sizeof(bad_flag),
                                      now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
        CHECK_EQ(result.error, CP_BOOT_ERR_WIRE);
        CHECK_EQ(result.state, CP_BOOT_STATE_FAILED);
    }
}

static void test_late_rejection_after_auth(void)
{
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 250u;
    static const uint8_t malformed_reject[10] = {
        0x40u, 0x40u, 0x00u, 0x0Au, 0x1Du, 0x03u, 0x00u, 0x05u,
        0x00u, 0x00u};
    static const uint8_t nonempty_flag[11] = {
        0x40u, 0x40u, 0x00u, 0x0Bu, 0x1Du, 0x03u, 0x00u, 0x05u,
        0x00u, 0x00u, 0x01u};

    memset(&auth, 0, sizeof(auth));
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out,
                              sizeof(out));
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_AUTHENTICATED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_SUCCEEDED);

    /* Late 1D03 is ignored before decoding, so a malformed frame cannot fail
     * an already authenticated session. */
    result = cp_bootstrap_receive(&session, malformed_reject,
                                  sizeof(malformed_reject), now, out,
                                  sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_SUCCEEDED);
    result = cp_bootstrap_receive(&session, nonempty_flag,
                                  sizeof(nonempty_flag), now, out,
                                  sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);

    /* Malformed late authentication duplicates are inapplicable before
     * decoding and must not turn a valid session into AUTH_FAILED. */
    {
        const uint8_t late_ids[] = {0x02u, 0x05u, 0x00u};
        uint8_t malformed_auth[] = {
            0x40u, 0x40u, 0x00u, 0x0Au, 0xAAu, 0x00u,
            0x00u, 0x05u, 0x00u, 0x00u};
        uint64_t generation = session.generation;
        int cert_calls = auth.cert_calls;
        int signature_calls = auth.signature_calls;
        size_t i;
        for (i = 0u; i < sizeof(late_ids); i++) {
            malformed_auth[5] = late_ids[i];
            result = cp_bootstrap_receive(&session, malformed_auth,
                                          sizeof(malformed_auth), now, out,
                                          sizeof(out));
            CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
            CHECK_EQ(result.error, CP_BOOT_OK);
            CHECK_EQ(result.out_len, 0u);
            CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
            CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_SUCCEEDED);
            CHECK_EQ(session.generation, generation);
            CHECK_EQ(auth.cert_calls, cert_calls);
            CHECK_EQ(auth.signature_calls, signature_calls);
        }
    }

    /* The session remains able to make a normal bounded offer. */
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_wpa2));
    CHECK_EQ(result.state, CP_BOOT_STATE_AWAITING_RECEIVER);
    CHECK(!result.projection_active);
}

static void test_authentication(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    policy_fixture policy;
    auth_fixture auth;
    cp_boot_auth_provider provider;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 300u;

    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_AUTH_PROVIDER);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_FAILED);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_FAILED);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_PENDING);
    CHECK_EQ(result.error, CP_BOOT_OK);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(auth.cert_calls, 0);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    CHECK_EQ(auth.cert_calls, 0);
    result = cp_bootstrap_tick(&session, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    CHECK(frame_equals(out, result.out_len, k_aa01));
    CHECK_EQ(auth.cert_calls, 1);
    result = feed(&session, k_aa02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    CHECK(frame_equals(out, result.out_len, k_aa03));
    result = feed(&session, k_aa05, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_SUCCEEDED);

    /* Interleaved auth-first order: both AA00 and AA02 arrive before 1D02.
     * Both are buffered, answered in protocol order after identification, and
     * credentials still wait for the phone's AA05. */
    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    CHECK_EQ(feed(&session, k_aa02, now, out, sizeof(out)).event,
             CP_BOOT_EV_AUTH_PENDING);
    CHECK_EQ(feed(&session, k_aa00, now, out, sizeof(out)).event,
             CP_BOOT_EV_AUTH_PENDING);
    CHECK_EQ(auth.cert_calls, 0);
    CHECK_EQ(feed(&session, k_1d00, now, out, sizeof(out)).event,
             CP_BOOT_EV_IDENTIFICATION_SENT);
    CHECK_EQ(feed(&session, k_1d02, now, out, sizeof(out)).event,
             CP_BOOT_EV_IDENTIFIED);
    CHECK_EQ(auth.cert_calls, 0);
    result = cp_bootstrap_tick(&session, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    CHECK_EQ(auth.cert_calls, 1);
    result = cp_bootstrap_tick(&session, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    CHECK_EQ(auth.signature_calls, 1);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_RESPONSE_SENT);
    CHECK_EQ(feed(&session, k_aa05, now, out, sizeof(out)).event,
             CP_BOOT_EV_AUTHENTICATED);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_aa05, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_FAILED);

    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    CHECK(frame_equals(out, result.out_len, k_aa01));
    CHECK_EQ(auth.cert_calls, 1);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_CERT_SENT);
    result = feed(&session, k_aa05, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_FAILED);

    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    CHECK(frame_equals(out, result.out_len, k_aa01));
    result = feed(&session, k_aa02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    CHECK(frame_equals(out, result.out_len, k_aa03));
    CHECK_EQ(auth.signature_calls, 1);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_RESPONSE_SENT);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_IDENTIFIED);
    result = feed(&session, k_aa05, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_SUCCEEDED);
    CHECK(!cp_bootstrap_projection_active(&session));
    CHECK_EQ(auth.cert_calls, 2);
    CHECK_EQ(auth.signature_calls, 1);

    result = feed(&session, k_aa04, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_AUTH_FAILED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_FAILED);

    memset(&auth, 0, sizeof(auth));
    auth.cert_rc = -1;
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_AUTH_PROVIDER);
    CHECK_EQ(result.out_len, 0u);

    memset(&auth, 0, sizeof(auth));
    auth.signature_rc = -1;
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(&session, k_aa02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_AUTH_PROVIDER);
    CHECK_EQ(result.out_len, 0u);

    memset(&provider, 0, sizeof(provider));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    provider.context = &auth;
    CHECK_EQ(cp_bootstrap_set_auth_provider(&session, &provider),
             CP_BOOT_OK);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK_EQ(result.error, CP_BOOT_ERR_AUTH_PROVIDER);
}

static void test_wifi_config_flow(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    policy_fixture policy;
    auth_fixture auth;
    cp_boot_ap_readback readback;
    uint8_t out[512];
    cp_boot_result result;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *nested;
    size_t nested_len;
    uint64_t now = 400u;

    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    result = feed(&session, k_1d02, now, out, sizeof(out));
    result = feed(&session, k_5702, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    CHECK_EQ(result.out_len, 0u);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out,
                              sizeof(out));
    result = feed(&session, k_5702, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_WIFI_CONFIG_SENT);
    CHECK(frame_equals(out, result.out_len, k_wifi_config));
    CHECK_EQ(find_param(out, result.out_len, 3u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 1u);
    CHECK_EQ(payload[0], 2u);

    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "CarPlay-AP";
    readback.channel = 149u;
    readback.security = CP_WIRE_WIFI_WPA3;
    readback.link_local = "fe80::2";
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    result = feed(&session, k_5702, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_WIFI_CONFIG_SENT);
    CHECK_EQ(find_param(out, result.out_len, 1u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 11u);
    CHECK(memcmp(payload, "CarPlay-AP\0", 11u) == 0);
    CHECK_EQ(find_param(out, result.out_len, 3u, &payload, &payload_len), 1);
    CHECK_EQ(payload[0], 2u);
    CHECK_EQ(find_param(out, result.out_len, 4u, &payload, &payload_len), 1);
    CHECK_EQ(payload[0], 149u);

    {
        static const uint8_t malformed[10] = {
            0x40u, 0x40u, 0x00u, 0x0Au, 0x57u, 0x02u, 0x00u, 0x04u,
            0x00u, 0x00u};
        result = cp_bootstrap_receive(&session, malformed, sizeof(malformed),
                                      now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
        CHECK_EQ(result.error, CP_BOOT_ERR_WIRE);
    }

    /* Identification names the on-air SSID when readback was published first. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "CarPlay-AP";
    readback.channel = 149u;
    readback.security = CP_WIRE_WIFI_WPA2;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    CHECK_EQ(find_param(out, result.out_len, 24u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 1u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 11u);
    CHECK(memcmp(nested, "CarPlay-AP\0", 11u) == 0);
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(&session, k_aa02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    result = feed(&session, k_aa05, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    result = feed(&session, k_5702, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_WIFI_CONFIG_SENT);
    CHECK_EQ(find_param(out, result.out_len, 1u, &payload, &payload_len), 1);
    CHECK_EQ(payload_len, 11u);
    CHECK(memcmp(payload, "CarPlay-AP\0", 11u) == 0);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_readback));
}

static void authenticate_only(cp_bootstrap *session, policy_fixture *policy,
                              auth_fixture *auth, uint64_t *now_ms,
                              uint8_t *out, size_t out_cap)
{
    cp_boot_result result;

    install_policy(session, policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(session, auth);
    result = feed(session, k_1d00, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(session, k_1d02, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = feed(session, k_aa00, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(session, k_aa02, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    result = feed(session, k_aa05, *now_ms, out, out_cap);
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    *now_ms += 10u;
}

static void test_start_gating_and_ap(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    policy_fixture policy;
    auth_fixture auth;
    cp_boot_ap_readback readback;
    uint8_t out[512];
    cp_boot_result result;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *nested;
    size_t nested_len;
    uint64_t now = 500u;

    memset(&auth, 0, sizeof(auth));
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    CHECK_EQ(result.out_len, 0u);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    CHECK(!result.projection_active);

    CHECK_EQ(cp_bootstrap_ap_state(&session), CP_BOOT_AP_UNKNOWN);
    result = cp_bootstrap_tick(&session, now + 500u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);

    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "SCOURGE-AP";
    readback.channel = 36u;
    readback.security = CP_WIRE_WIFI_WPA2;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now + 600u,
                                          out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_wpa2));
    CHECK_EQ(result.state, CP_BOOT_STATE_AWAITING_RECEIVER);
    CHECK_EQ(cp_bootstrap_ap_state(&session), CP_BOOT_AP_READY);
    CHECK(!result.projection_active);
    CHECK(!cp_bootstrap_projection_active(&session));
    result = cp_bootstrap_tick(&session, now + 700u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);

    result = feed(&session, k_4300_wireless, now + 800u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_DUPLICATE);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AWAITING_RECEIVER);

    result = feed(&session, k_4300_false, now + 900u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AVAILABILITY_FALSE);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);

    result = feed(&session, k_4300_wireless, now + 1000u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_wpa2));

    /* AP readiness timeout is bounded and clears the pending offer. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    result = cp_bootstrap_tick(&session, now + 1000u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_TIMEOUT);
    CHECK_EQ(result.error, CP_BOOT_ERR_TIMEOUT);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    set_ap_ready(&session, now + 1100u, out, sizeof(out));
    result = cp_bootstrap_tick(&session, now + 1200u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);

    /* A ready readback cannot bypass an expired pending deadline. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "SCOURGE-AP";
    readback.channel = 36u;
    readback.security = CP_WIRE_WIFI_WPA2;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now + 1000u,
                                          out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_TIMEOUT);
    CHECK_EQ(result.error, CP_BOOT_ERR_TIMEOUT);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    result = cp_bootstrap_tick(&session, now + 1001u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);

    /* Availability going false clears a pending offer without a start. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    result = feed(&session, k_4300_false, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AVAILABILITY_FALSE);
    CHECK_EQ(result.out_len, 0u);
    set_ap_ready(&session, now + 2u, out, sizeof(out));
    result = cp_bootstrap_tick(&session, now + 3u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);

    /* A not-ready readback leaves the offer pending, then times out. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    memset(&readback, 0, sizeof(readback));
    readback.on_air = false;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now + 100u,
                                          out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_NOT_READY);
    CHECK_EQ(cp_bootstrap_ap_state(&session), CP_BOOT_AP_NOT_READY);
    result = cp_bootstrap_tick(&session, now + 999u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = cp_bootstrap_tick(&session, now + 1000u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_TIMEOUT);

    /* Invalid readbacks never claim readiness. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    {
        static const char *const bad_ssids[] = {
            "",
            "0123456789012345678901234567890123",
        };
        size_t i;
        for (i = 0; i < sizeof(bad_ssids) / sizeof(bad_ssids[0]); i++) {
            memset(&readback, 0, sizeof(readback));
            readback.on_air = true;
            readback.ssid = bad_ssids[i];
            readback.channel = 36u;
            readback.security = CP_WIRE_WIFI_WPA2;
            result = cp_bootstrap_set_ap_readback(
                &session, &readback, now, out, sizeof(out));
            CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
            CHECK_EQ(result.error, CP_BOOT_ERR_CONFIG);
        }
    }
    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "SCOURGE-AP";
    readback.channel = 0u;
    readback.security = CP_WIRE_WIFI_WPA2;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    readback.channel = 36u;
    readback.security = (cp_wire_wifi_security)99;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    readback.security = CP_WIRE_WIFI_WPA2;
    readback.link_local = "ff02::1";
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    CHECK_EQ(result.error, CP_BOOT_ERR_CONFIG);
    readback.link_local = "::fe80:1";
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    CHECK_EQ(result.error, CP_BOOT_ERR_CONFIG);
    readback.link_local = "::febf:1";
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    CHECK_EQ(result.error, CP_BOOT_ERR_CONFIG);
    readback.link_local = NULL;

    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);

    /* Readback selects the security enum for 0x4301. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "SCOURGE-AP";
    readback.channel = 36u;
    readback.security = CP_WIRE_WIFI_WPA2_WPA3;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_transition));

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    readback.security = CP_WIRE_WIFI_WPA3;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_wpa3));

    /* A readback link-local overrides the configured address. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    memset(&readback, 0, sizeof(readback));
    readback.on_air = true;
    readback.ssid = "SCOURGE-AP";
    readback.channel = 36u;
    readback.security = CP_WIRE_WIFI_WPA2;
    readback.link_local = "fe80::2";
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK_EQ(find_param(out, result.out_len, 1u, &payload, &payload_len), 1);
    CHECK_EQ(group_param(payload, payload_len, 3u, &nested, &nested_len), 1);
    CHECK_EQ(nested_len, 8u);
    CHECK(memcmp(nested, "fe80::2\0", 8u) == 0);

    /* The channel and SSID come from the on-air readback, not config. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    readback.ssid = "CarPlay-AP";
    readback.channel = 149u;
    readback.security = CP_WIRE_WIFI_WPA2;
    readback.link_local = NULL;
    result = cp_bootstrap_set_ap_readback(&session, &readback, now, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_readback));

    /* Conservative availability policy: absent optional fields are not
     * assumed true.  A wired-only or wireless-without-available message
     * refuses the offer. */
    {
        static const char *k_4300_wired_only =
            "404000194300001300000005000001000a00017573622d3100";
        static const char *k_4300_wireless_no_available =
            "404000204300001a00010016000141413a42423a43433a44443a45453a"
            "464600";

        cfg = base_config();
        CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
        authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
        set_ap_ready(&session, now, out, sizeof(out));
        result = feed(&session, k_4300_wired_only, now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_AVAILABILITY_FALSE);
        CHECK_EQ(result.out_len, 0u);
        CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
        result = feed(&session, k_4300_wireless_no_available, now, out,
                      sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_AVAILABILITY_FALSE);
        CHECK_EQ(result.out_len, 0u);
        result = feed(&session, k_4300_wireless, now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    }

    /* An explicit 4E0D "unavailable" blocks an offer. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4e0d_unavailable, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_WIRELESS_STATUS);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK_EQ(result.out_len, 0u);
    result = feed(&session, k_4e0d_available, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_WIRELESS_STATUS);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
}

static void test_offer_deadline_and_policy(void)
{
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 550u;

    memset(&auth, 0, sizeof(auth));

    /* The post-start receiver offer expires at the same bounded wait as AP
     * readiness and never leaves an active offer behind. */
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK_EQ(result.state, CP_BOOT_STATE_AWAITING_RECEIVER);
    CHECK(!cp_bootstrap_projection_active(&session));
    result = cp_bootstrap_tick(&session, now + 999u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(cp_bootstrap_state(&session),
             CP_BOOT_STATE_AWAITING_RECEIVER);
    result = cp_bootstrap_tick(&session, now + 1000u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_TIMEOUT);
    CHECK_EQ(result.error, CP_BOOT_ERR_TIMEOUT);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    CHECK(!result.projection_active);
    CHECK(!cp_bootstrap_projection_active(&session));
    result = cp_bootstrap_tick(&session, now + 1001u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);

    /* A newer availability after expiry makes a fresh offer, not a stale
     * duplicate. */
    result = feed(&session, k_4300_wireless, now + 1002u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_wpa2));

    /* Consent revoked after START is observed on the next bounded tick. */
    policy.consent = CP_BOOT_CONSENT_DENIED;
    result = cp_bootstrap_tick(&session, now + 1003u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    CHECK(!cp_bootstrap_projection_active(&session));
    result = cp_bootstrap_tick(&session, now + 1004u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);

    /* USB becoming active after START invalidates the offer without a frame. */
    policy.consent = CP_BOOT_CONSENT_GRANTED;
    result = feed(&session, k_4300_wireless, now + 1005u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    policy.usb_active = true;
    result = cp_bootstrap_tick(&session, now + 1006u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_USB_ACTIVE);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);

    /* A consent provider that is merely still pending keeps the bounded offer
     * alive but never claims projection; a later grant does not duplicate the
     * start. */
    policy.usb_active = false;
    result = feed(&session, k_4300_wireless, now + 1007u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    policy.consent = CP_BOOT_CONSENT_PENDING;
    result = cp_bootstrap_tick(&session, now + 1008u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(cp_bootstrap_state(&session),
             CP_BOOT_STATE_AWAITING_RECEIVER);
    CHECK(!cp_bootstrap_projection_active(&session));
    policy.consent = CP_BOOT_CONSENT_GRANTED;
    result = cp_bootstrap_tick(&session, now + 1009u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);
    CHECK_EQ(cp_bootstrap_state(&session),
             CP_BOOT_STATE_AWAITING_RECEIVER);

    /* Availability at/after the deadline replaces the expired offer even if
     * no tick was delivered in between. */
    result = feed(&session, k_4300_wireless, now + 2007u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(frame_equals(out, result.out_len, k_start_wpa2));
}

static void test_offer_hold(void)
{
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    policy_fixture policy;
    auth_fixture auth;
    cp_boot_ap_readback readback;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 700u;
    uint64_t generation;

    memset(&auth, 0, sizeof(auth));

    /* No offer: a hold cannot be created from nothing. */
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    CHECK(!cp_bootstrap_offer_held(&session));

    /* Identified but not authenticated: refused.  Authenticated but no offer:
     * still refused. */
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    install_auth(&session, &auth);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK(!cp_bootstrap_offer_held(&session));
    result = feed(&session, k_aa00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(&session, k_aa02, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    result = feed(&session, k_aa05, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    now += 10u;
    result = cp_bootstrap_hold_offer(&session, cp_bootstrap_generation(&session),
                                     now);
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK(!cp_bootstrap_offer_held(&session));

    /* Create the bounded offer. */
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK_EQ(result.state, CP_BOOT_STATE_AWAITING_RECEIVER);
    generation = cp_bootstrap_generation(&session);

    /* A duplicate wire message never creates the hold. */
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_DUPLICATE);
    CHECK(!cp_bootstrap_offer_held(&session));

    /* Wrong generation is refused without disturbing the offer. */
    result = cp_bootstrap_hold_offer(&session, generation + 1u, now);
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);

    /* A matching hold retires only the deadline; the offer and its bounded
     * policy checks remain in place. */
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.error, CP_BOOT_OK);
    CHECK(cp_bootstrap_offer_held(&session));
    now += 100000u;
    result = cp_bootstrap_tick(&session, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);
    CHECK(cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_AWAITING_RECEIVER);
    CHECK(!cp_bootstrap_projection_active(&session));

    /* A new wired id while held must reach the USB callback; policy that is
     * active only for the new id tears the offer down immediately. */
    policy.usb_true_id = "usb-2";
    result = feed(&session, k_4300_both_usb2, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_USB_ACTIVE);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK_EQ(result.out_len, 0u);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    CHECK(session.have_phone_usb_id);
    CHECK(strcmp(session.phone_usb_id, "usb-2") == 0);
    CHECK(!cp_bootstrap_projection_active(&session));
    policy.usb_true_id = NULL;
    now += 10u;

    /* USB becoming active while held is noticed on the bounded tick. */
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK(cp_bootstrap_offer_held(&session));
    policy.usb_active = true;
    result = cp_bootstrap_tick(&session, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_USB_ACTIVE);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    policy.usb_active = false;
    now += 10u;

    /* Consent denial while held invalidates immediately on a tick. */
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    policy.consent = CP_BOOT_CONSENT_DENIED;
    result = cp_bootstrap_tick(&session, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    policy.consent = CP_BOOT_CONSENT_GRANTED;
    now += 10u;

    /* Wireless withdrawal clears both the offer and the hold. */
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = feed(&session, k_4300_false, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AVAILABILITY_FALSE);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    now += 10u;

    /* Cancel clears the hold and the offer. */
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = cp_bootstrap_cancel(&session, now + 1u);
    CHECK_EQ(result.event, CP_BOOT_EV_CANCELLED);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    now += 10u;

    /* AP-scope invalidation drops the hold but leaves the offer to its normal
     * bounded deadline, which still expires it. */
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    memset(&readback, 0, sizeof(readback));
    readback.on_air = false;
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_set_ap_readback(&session, &readback, now + 1u, out,
                                          sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AP_NOT_READY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);
    set_ap_ready(&session, now + 2u, out, sizeof(out));
    result = cp_bootstrap_tick(&session, session.offer_deadline_ms, out,
                               sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_TIMEOUT);
    CHECK(!session.offer_active);
    now = session.last_now_ms;

    /* Transport reset clears the hold even while it is valid. */
    result = feed(&session, k_4300_wireless, now + 10u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now + 10u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = cp_bootstrap_transport_reset(&session, now + 11u);
    CHECK_EQ(result.event, CP_BOOT_EV_TRANSPORT_RESET);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_DISCONNECTED);
    CHECK(!cp_bootstrap_projection_active(&session));
    puts("PASS: offer hold trust, expiry and invalidation lifecycles");
}

static void test_hold_ap_scope_and_release(void)
{
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 800u;
    uint64_t generation;
    uint64_t deadline;

    memset(&auth, 0, sizeof(auth));
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    deadline = session.offer_deadline_ms;
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);

    /* Direct release succeeds and is idempotent; the offer, deadline and
     * generation are untouched. */
    result = cp_bootstrap_release_offer_hold(&session, now + 1u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.error, CP_BOOT_OK);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);
    result = cp_bootstrap_release_offer_hold(&session, now + 2u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.error, CP_BOOT_OK);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);

    /* Only a fresh caller attestation re-holds the same bounded offer. */
    result = cp_bootstrap_hold_offer(&session, generation, now + 3u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK(cp_bootstrap_offer_held(&session));

    /* A byte-identical on-air readback is not an AP scope change. */
    set_ap_ready(&session, now + 4u, out, sizeof(out));
    CHECK(cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);

    /* SSID change. */
    result = set_ap_readback_fields(&session, now + 5u, out, sizeof(out),
                                    "SCOURGE-AP-2", 36u, CP_WIRE_WIFI_WPA2,
                                    NULL);
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    CHECK_EQ(result.out_len, 0u);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);

    /* Channel change. */
    result = cp_bootstrap_hold_offer(&session, generation, now + 6u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = set_ap_readback_fields(&session, now + 7u, out, sizeof(out),
                                    "SCOURGE-AP-2", 40u, CP_WIRE_WIFI_WPA2,
                                    NULL);
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);

    /* Security change. */
    result = cp_bootstrap_hold_offer(&session, generation, now + 8u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = set_ap_readback_fields(&session, now + 9u, out, sizeof(out),
                                    "SCOURGE-AP-2", 40u, CP_WIRE_WIFI_WPA3,
                                    NULL);
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);

    /* Link-local change. */
    result = cp_bootstrap_hold_offer(&session, generation, now + 10u);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = set_ap_readback_fields(&session, now + 11u, out, sizeof(out),
                                    "SCOURGE-AP-2", 40u, CP_WIRE_WIFI_WPA3,
                                    "fe80::2");
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(session.offer_deadline_ms, deadline);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);

    /* Re-publishing the new scope does not silently re-hold it. */
    result = set_ap_readback_fields(&session, now + 12u, out, sizeof(out),
                                    "SCOURGE-AP-2", 40u, CP_WIRE_WIFI_WPA3,
                                    "fe80::2");
    CHECK_EQ(result.event, CP_BOOT_EV_AP_READY);
    CHECK(!cp_bootstrap_offer_held(&session));

    /* The offer is still bounded by its original deadline and never starts. */
    result = cp_bootstrap_tick(&session, deadline, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_TIMEOUT);
    CHECK_EQ(result.error, CP_BOOT_ERR_TIMEOUT);
    CHECK_EQ(result.out_len, 0u);
    CHECK(!session.offer_active);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK_EQ(cp_bootstrap_generation(&session), generation + 1u);

    /* A stale generation cannot re-create a hold. */
    result = cp_bootstrap_hold_offer(&session, generation, deadline);
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK(!cp_bootstrap_offer_held(&session));
    puts("PASS: hold release and changed AP scope stay bounded");
}

static void test_hold_failure_invalidation(void)
{
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 900u;
    uint64_t generation;

    memset(&auth, 0, sizeof(auth));

    /* Authentication failure after a hold clears both offer and hold. */
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = feed(&session, k_aa04, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_FAILED);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_FAILED);

    /* Wireless withdrawal (4E0D) clears both. */
    now = 1000u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = feed(&session, k_4e0d_unavailable, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_WIRELESS_STATUS);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);

    /* A changed transport identity (4E0E) is a new identity scope. */
    now = 1100u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = feed(&session, k_4e0e_usb2, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_PHONE_IDENTITY);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);
    CHECK(session.have_phone_usb_id);
    CHECK(strcmp(session.phone_usb_id, "usb-2") == 0);

    /* A late identification rejection cannot occur while a hold is valid:
     * the hold requires an identified, authenticated AWAITING_RECEIVER
     * session.  Such a frame is ignored and must preserve the hold rather
     * than fabricate a failure.  fail_identification() itself still clears
     * any offer/hold through clear_offer(). */
    now = 1200u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    identify_and_authenticate(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = feed(&session, k_1d03_bt, now + 1u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IGNORED);
    CHECK(cp_bootstrap_offer_held(&session));
    CHECK(session.offer_active);
    CHECK_EQ(cp_bootstrap_generation(&session), generation);
    CHECK_EQ(cp_bootstrap_cancel(&session, now + 2u).event,
             CP_BOOT_EV_CANCELLED);
    CHECK(!cp_bootstrap_offer_held(&session));
    CHECK(!session.offer_active);

    /* Identification failure is terminal before any offer/hold can exist.  It
     * still exercises the same clear_offer() invalidation, and a later hold
     * attempt must be refused. */
    now = 1250u;
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_1d00, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_RETRY);
    result = feed(&session, k_1d03_team, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_REJECTED);
    CHECK_EQ(cp_bootstrap_state(&session), CP_BOOT_STATE_FAILED);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_hold_offer(&session, generation, now);
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK(!cp_bootstrap_offer_held(&session));
    puts("PASS: hold clears through failure and withdrawal lifecycles");
}

static void test_identity_and_policy(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 600u;

    memset(&auth, 0, sizeof(auth));

    /* A matching 4E0E identity starts; a mismatch never does. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4e0e, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_PHONE_IDENTITY);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4e0e, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_PHONE_IDENTITY);
    {
        static const char *const other_wireless =
            "404000254300001f000100050000010016000142423a42423a43433a4444"
            "3a45453a464600";
        result = feed(&session, other_wireless, now, out, sizeof(out));
        CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
        CHECK_EQ(result.error, CP_BOOT_ERR_IDENTITY);
        CHECK_EQ(result.out_len, 0u);
    }

    /* A wired offer without a USB arbiter is not safe to override. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    CHECK_EQ(cp_bootstrap_set_policy(&session, NULL, NULL, NULL), CP_BOOT_OK);
    result = feed(&session, k_4300_both, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_USB_ACTIVE);
    CHECK_EQ(result.error, CP_BOOT_ERR_POLICY);
    CHECK_EQ(result.out_len, 0u);

    /* An injected arbiter can allow or block the wireless start. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_4300_both, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    CHECK(policy.usb_calls >= 1);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, true);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_USB_ACTIVE);
    CHECK_EQ(result.out_len, 0u);

    /* USB becoming active cancels a pending start and no stale frame fires. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_GRANTED, false);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    policy.usb_active = true;
    result = cp_bootstrap_tick(&session, now + 100u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_USB_ACTIVE);
    CHECK_EQ(result.out_len, 0u);
    policy.usb_active = false;
    set_ap_ready(&session, now + 200u, out, sizeof(out));
    result = cp_bootstrap_tick(&session, now + 300u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    result = feed(&session, k_4300_wireless, now + 400u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);

    /* Consent is deny-by-default and injectable. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    CHECK_EQ(cp_bootstrap_set_policy(&session, NULL, NULL, NULL), CP_BOOT_OK);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);
    CHECK_EQ(result.out_len, 0u);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_DENIED, false);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);

    /* An unexpected provider value fails closed, not open. */
    install_policy(&session, &policy, (cp_boot_consent)99, false);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);
    CHECK_EQ(result.out_len, 0u);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_PENDING, false);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    result = cp_bootstrap_tick(&session, now + 100u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    policy.consent = CP_BOOT_CONSENT_GRANTED;
    result = cp_bootstrap_tick(&session, now + 200u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_PENDING, false);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    policy.consent = CP_BOOT_CONSENT_DENIED;
    result = cp_bootstrap_tick(&session, now + 100u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);
    CHECK_EQ(result.out_len, 0u);

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    install_policy(&session, &policy, CP_BOOT_CONSENT_PENDING, false);
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    policy.consent = (cp_boot_consent)99;
    result = cp_bootstrap_tick(&session, now + 100u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_CONSENT_DENIED);
    CHECK_EQ(result.out_len, 0u);
}

static void test_cancel_reset_and_stale(void)
{
    cp_bootstrap session;
    cp_boot_config cfg;
    policy_fixture policy;
    auth_fixture auth;
    uint8_t out[512];
    cp_boot_result result;
    uint64_t now = 700u;
    uint64_t generation;

    memset(&auth, 0, sizeof(auth));

    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_cancel(&session, now + 10u);
    CHECK_EQ(result.event, CP_BOOT_EV_CANCELLED);
    CHECK(cp_bootstrap_generation(&session) > generation);
    set_ap_ready(&session, now + 20u, out, sizeof(out));
    result = cp_bootstrap_tick(&session, now + 30u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);
    result = feed(&session, k_4300_wireless, now + 40u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);

    /* Cancelling an unacknowledged start allows a fresh offer, not a stale
     * frame from the previous generation. */
    result = cp_bootstrap_cancel(&session, now + 50u);
    CHECK_EQ(result.event, CP_BOOT_EV_CANCELLED);
    CHECK_EQ(result.state, CP_BOOT_STATE_AUTHENTICATED);
    CHECK(!result.projection_active);
    result = cp_bootstrap_tick(&session, now + 60u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    CHECK_EQ(result.out_len, 0u);
    result = feed(&session, k_4300_wireless, now + 70u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);

    /* Reset invalidates authentication and identification. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    set_ap_ready(&session, now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);
    result = cp_bootstrap_transport_reset(&session, now + 10u);
    CHECK_EQ(result.event, CP_BOOT_EV_TRANSPORT_RESET);
    CHECK_EQ(result.state, CP_BOOT_STATE_DISCONNECTED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_NONE);
    CHECK(!cp_bootstrap_projection_active(&session));
    result = feed(&session, k_4300_wireless, now + 20u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    CHECK_EQ(result.error, CP_BOOT_ERR_ORDER);
    result = feed(&session, k_5702, now + 20u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_REFUSED);
    result = feed(&session, k_1d00, now + 30u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed(&session, k_1d02, now + 30u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = feed(&session, k_aa00, now + 30u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    result = feed(&session, k_aa02, now + 30u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    result = feed(&session, k_aa05, now + 30u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    result = feed(&session, k_4300_wireless, now + 40u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_SENT);

    /* Reset while pending leaves no deferred start. */
    cfg = base_config();
    CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
    authenticate_only(&session, &policy, &auth, &now, out, sizeof(out));
    result = feed(&session, k_4300_wireless, now, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_START_PENDING_AP);
    result = cp_bootstrap_transport_reset(&session, now + 10u);
    CHECK_EQ(result.event, CP_BOOT_EV_TRANSPORT_RESET);
    result = cp_bootstrap_tick(&session, now + 20u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);
    set_ap_ready(&session, now + 30u, out, sizeof(out));
    result = cp_bootstrap_tick(&session, now + 40u, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_NONE);

    /* The generation API is monotonic across explicit invalidation. */
    generation = cp_bootstrap_generation(&session);
    result = cp_bootstrap_cancel(&session, now + 50u);
    CHECK_EQ(result.event, CP_BOOT_EV_CANCELLED);
    CHECK(cp_bootstrap_generation(&session) > generation);

    result = cp_bootstrap_cancel(NULL, now);
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    result = cp_bootstrap_transport_reset(NULL, now);
    CHECK_EQ(result.event, CP_BOOT_EV_ERROR);
    CHECK(!cp_bootstrap_projection_active(NULL));
    CHECK_EQ(cp_bootstrap_state(NULL), CP_BOOT_STATE_DISCONNECTED);
    CHECK_EQ(cp_bootstrap_auth_state(NULL), CP_BOOT_AUTH_NONE);
}

static void test_malformed_tlv_corpus(void)
{
    typedef struct {
        const uint8_t *data;
        size_t len;
        uint16_t msg_id;
        cp_wire_status expected;
    } corpus_case;

    static const uint8_t tlv_zero[10] = {
        0x40u, 0x40u, 0x00u, 0x0Au, 0x4Eu, 0x0Du,
        0x00u, 0x00u, 0x00u, 0x00u};
    static const uint8_t tlv_short[10] = {
        0x40u, 0x40u, 0x00u, 0x0Au, 0x4Eu, 0x0Du,
        0x00u, 0x03u, 0x00u, 0x00u};
    static const uint8_t tlv_truncated[10] = {
        0x40u, 0x40u, 0x00u, 0x0Au, 0x4Eu, 0x0Du,
        0x00u, 0x08u, 0x00u, 0x00u};
    static const uint8_t tlv_trailing[12] = {
        0x40u, 0x40u, 0x00u, 0x0Cu, 0x4Eu, 0x0Du,
        0x00u, 0x05u, 0x00u, 0x00u, 0x01u, 0x55u};
    static const uint8_t bool_bad[11] = {
        0x40u, 0x40u, 0x00u, 0x0Bu, 0x4Eu, 0x0Du,
        0x00u, 0x05u, 0x00u, 0x00u, 0x02u};
    static const uint8_t bool_duplicate[16] = {
        0x40u, 0x40u, 0x00u, 0x10u, 0x4Eu, 0x0Du,
        0x00u, 0x05u, 0x00u, 0x00u, 0x01u,
        0x00u, 0x05u, 0x00u, 0x00u, 0x01u};
    static const uint8_t group_short[11] = {
        0x40u, 0x40u, 0x00u, 0x0Bu, 0x43u, 0x00u,
        0x00u, 0x05u, 0x00u, 0x01u, 0x55u};
    static const uint8_t group_nested_truncated[15] = {
        0x40u, 0x40u, 0x00u, 0x0Fu, 0x43u, 0x00u,
        0x00u, 0x09u, 0x00u, 0x01u,
        0x00u, 0x09u, 0x00u, 0x00u, 0x01u};
    static const uint8_t reject_truncated[10] = {
        0x40u, 0x40u, 0x00u, 0x0Au, 0x1Du, 0x03u,
        0x00u, 0x08u, 0x00u, 0x19u};
    static const uint8_t challenge_empty[10] = {
        0x40u, 0x40u, 0x00u, 0x0Au, 0xAAu, 0x02u,
        0x00u, 0x04u, 0x00u, 0x00u};
    static const uint8_t ids_no_nul[24] = {
        0x40u, 0x40u, 0x00u, 0x18u, 0x4Eu, 0x0Eu,
        0x00u, 0x0Au, 0x00u, 0x00u, 0x41u, 0x41u, 0x3Au, 0x42u,
        0x42u, 0x00u,
        0x00u, 0x08u, 0x00u, 0x01u, 0x75u, 0x73u, 0x62u, 0x31u};
    static const uint8_t unknown_truncated[12] = {
        0x40u, 0x40u, 0x00u, 0x0Cu, 0x4Eu, 0x0Eu,
        0x00u, 0x09u, 0x00u, 0x07u, 0xAAu, 0xBBu};
    static const uint8_t header_short_total[6] = {
        0x40u, 0x40u, 0x00u, 0x05u, 0x4Eu, 0x0Du};
    static const uint8_t header_big_total[6] = {
        0x40u, 0x40u, 0x10u, 0x01u, 0x4Eu, 0x0Du};
    static const uint8_t header_truncated[10] = {
        0x40u, 0x40u, 0x00u, 0x14u, 0x4Eu, 0x0Du,
        0x00u, 0x05u, 0x00u, 0x00u};
    static const corpus_case cases[] = {
        {tlv_zero, sizeof(tlv_zero), 0x4E0Du, CP_WIRE_ERR_TLV},
        {tlv_short, sizeof(tlv_short), 0x4E0Du, CP_WIRE_ERR_TLV},
        {tlv_truncated, sizeof(tlv_truncated), 0x4E0Du,
         CP_WIRE_ERR_TRUNCATED},
        {tlv_trailing, sizeof(tlv_trailing), 0x4E0Du, CP_WIRE_ERR_TLV},
        {bool_bad, sizeof(bool_bad), 0x4E0Du, CP_WIRE_ERR_TYPE},
        {bool_duplicate, sizeof(bool_duplicate), 0x4E0Du,
         CP_WIRE_ERR_DUPLICATE},
        {group_short, sizeof(group_short), 0x4300u, CP_WIRE_ERR_TLV},
        {group_nested_truncated, sizeof(group_nested_truncated), 0x4300u,
         CP_WIRE_ERR_TRUNCATED},
        {reject_truncated, sizeof(reject_truncated), 0x1D03u,
         CP_WIRE_ERR_TRUNCATED},
        {challenge_empty, sizeof(challenge_empty), 0xAA02u,
         CP_WIRE_ERR_RANGE},
        {ids_no_nul, sizeof(ids_no_nul), 0x4E0Eu, CP_WIRE_ERR_UTF8},
        {unknown_truncated, sizeof(unknown_truncated), 0x4E0Eu,
         CP_WIRE_ERR_TRUNCATED},
        {header_short_total, sizeof(header_short_total), 0x4E0Du,
         CP_WIRE_ERR_LENGTH},
        {header_big_total, sizeof(header_big_total), 0x4E0Du,
         CP_WIRE_ERR_LENGTH},
        {header_truncated, sizeof(header_truncated), 0x4E0Du,
         CP_WIRE_ERR_TRUNCATED},
    };
    cp_bootstrap session;
    cp_boot_config cfg = base_config();
    uint8_t out[512];
    unsigned round;
    size_t i;

    for (round = 0u; round < 32u; round++) {
        CHECK_EQ(cp_bootstrap_init(&session, &cfg), CP_BOOT_OK);
        for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
            cp_wire_view view;
            cp_wire_status st = cp_wire_open(cases[i].data, cases[i].len,
                                             &view);
            cp_boot_result result;

            if (st != CP_WIRE_OK) {
                CHECK_EQ(st, cases[i].expected);
                result = cp_bootstrap_receive(&session, cases[i].data,
                                              cases[i].len, 1u, out,
                                              sizeof(out));
                CHECK(result.event != CP_BOOT_EV_START_SENT);
                CHECK(!cp_bootstrap_projection_active(&session));
                continue;
            }
            CHECK_EQ(view.msg_id, cases[i].msg_id);
            switch (cases[i].msg_id) {
            case 0x4E0Du: {
                cp_wire_wireless_update update;
                st = cp_wire_decode_wireless_update(&view, &update);
                break;
            }
            case 0x4E0Eu: {
                cp_wire_transport_ids ids;
                st = cp_wire_decode_transport_ids(&view, &ids);
                break;
            }
            case 0x4300u: {
                cp_wire_availability availability;
                st = cp_wire_decode_availability(&view, &availability);
                break;
            }
            case 0x1D03u: {
                cp_wire_identification_rejection rejection;
                st = cp_wire_decode_identification_rejection(&view,
                                                             &rejection);
                break;
            }
            case 0xAA02u: {
                const uint8_t *challenge = NULL;
                size_t challenge_len = 0u;
                st = cp_wire_decode_challenge_request(&view, &challenge,
                                                      &challenge_len);
                break;
            }
            default:
                st = CP_WIRE_ERR_MSG_ID;
                break;
            }
            CHECK_EQ(st, cases[i].expected);
            result = cp_bootstrap_receive(&session, cases[i].data,
                                          cases[i].len, 1u, out,
                                          sizeof(out));
            CHECK(result.event != CP_BOOT_EV_START_SENT);
            CHECK(!cp_bootstrap_projection_active(&session));
        }
    }
}

static void emit_hex(const char *name, const uint8_t *data, size_t len)
{
    size_t i;

    printf("%s ", name);
    for (i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    putchar('\n');
}

/* Fixture output for the independent Python parser.  The passphrase here is
 * deliberately fake and must never be replaced with a real credential. */
static int emit_fixtures(void)
{
    cp_wire_identification id = base_ident();
    cp_wire_wifi_config wifi;
    cp_wire_start_session start;
    uint8_t out[512];
    size_t out_len = 0u;
    uint8_t certificate[] = {0x01u, 0x02u, 0xFFu};
    uint8_t signature[] = {0xDEu, 0xADu, 0xBEu, 0xEFu};

    if (cp_wire_encode_identification(&id, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("ident", out, out_len);

    id.app_match_team_id = NULL;
    if (cp_wire_encode_identification(&id, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("ident_no_team", out, out_len);

    wifi.ssid = "SCOURGE-AP";
    wifi.passphrase = "Wireless-Test-1";
    wifi.channel = 36u;
    if (cp_wire_encode_wifi_config(&wifi, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("wifi", out, out_len);

    start = base_start();
    if (cp_wire_encode_start_session(&start, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("start_wpa2", out, out_len);

    start.security = CP_WIRE_WIFI_WPA2_WPA3;
    if (cp_wire_encode_start_session(&start, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("start_transition", out, out_len);

    start.security = CP_WIRE_WIFI_WPA3;
    if (cp_wire_encode_start_session(&start, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("start_wpa3", out, out_len);

    start = base_start();
    start.ssid = "CarPlay-AP";
    start.channel = 149u;
    if (cp_wire_encode_start_session(&start, out, sizeof(out), &out_len) !=
        CP_WIRE_OK) {
        return 1;
    }
    emit_hex("start_readback", out, out_len);

    if (cp_wire_encode_auth_certificate(certificate, sizeof(certificate), out,
                                        sizeof(out), &out_len) != CP_WIRE_OK) {
        return 1;
    }
    emit_hex("aa01", out, out_len);
    if (cp_wire_encode_auth_response(signature, sizeof(signature), out,
                                     sizeof(out), &out_len) != CP_WIRE_OK) {
        return 1;
    }
    emit_hex("aa03", out, out_len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--emit-fixtures") == 0) {
        return emit_fixtures();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--emit-fixtures]\n", argv[0]);
        return 2;
    }

    test_headers_and_tlv();
    test_identification_encoding();
    test_wifi_and_start_encoding();
    test_inbound_decoding();
    test_init_validation();
    test_identification_flow();
    test_identification_rejection();
    test_late_rejection_after_auth();
    test_authentication();
    test_wifi_config_flow();
    test_start_gating_and_ap();
    test_offer_deadline_and_policy();
    test_offer_hold();
    test_hold_ap_scope_and_release();
    test_hold_failure_invalidation();
    test_identity_and_policy();
    test_cancel_reset_and_stale();
    test_malformed_tlv_corpus();

    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("carplay bootstrap: %d checks passed\n", checks);
    return 0;
}
